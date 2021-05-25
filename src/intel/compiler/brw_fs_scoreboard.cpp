/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file brw_fs_scoreboard.cpp
 *
 * Gen12+ hardware lacks the register scoreboard logic that used to guarantee
 * data coherency between register reads and writes in previous generations.
 * This lowering pass runs after register allocation in order to make up for
 * it.
 *
 * It works by performing global dataflow analysis in order to determine the
 * set of potential dependencies of every instruction in the shader, and then
 * inserts any required SWSB annotations and additional SYNC instructions in
 * order to guarantee data coherency.
 *
 * WARNING - Access of the following (rarely used) ARF registers is not
 *           tracked here, and require the RegDist SWSB annotation to be set
 *           to 1 by the generator in order to avoid data races:
 *
 *  - sp stack pointer
 *  - sr0 state register
 *  - cr0 control register
 *  - ip instruction pointer
 *  - tm0 timestamp register
 *  - dbg0 debug register
 *
 * The following ARF registers don't need to be tracked here because data
 * coherency is still provided transparently by the hardware:
 *
 *  - f0-1 flag registers
 *  - n0 notification register
 *  - tdr0 thread dependency register
 */

#include "brw_fs.h"
#include "brw_cfg.h"

using namespace brw;

namespace {
   /**
    * In-order instruction accounting.
    * @{
    */

   /**
    * Number of in-order hardware instructions contained in this IR
    * instruction.  This determines the increment applied to the RegDist
    * counter calculated for any ordered dependency that crosses this
    * instruction.
    */
   unsigned
   ordered_unit(const fs_inst *inst)
   {
      switch (inst->opcode) {
      case BRW_OPCODE_SYNC:
      case BRW_OPCODE_DO:
      case SHADER_OPCODE_UNDEF:
      case SHADER_OPCODE_HALT_TARGET:
      case FS_OPCODE_SCHEDULING_FENCE:
         return 0;
      default:
         /* Note that the following is inaccurate for virtual instructions
          * that expand to more in-order instructions than assumed here, but
          * that can only lead to suboptimal execution ordering, data
          * coherency won't be impacted.  Providing exact RegDist counts for
          * each virtual instruction would allow better ALU performance, but
          * it would require keeping this switch statement in perfect sync
          * with the generator in order to avoid data corruption.  Lesson is
          * (again) don't use virtual instructions if you want optimal
          * scheduling.
          */
         return is_unordered(inst) ? 0 : 1;
      }
   }

   /**
    * Type for an instruction counter that increments for in-order
    * instructions only, arbitrarily denoted 'jp' throughout this lowering
    * pass in order to distinguish it from the regular instruction counter.
    */
   typedef int ordered_address;

   /**
    * Return the number of instructions in the program.
    */
   unsigned
   num_instructions(const backend_shader *shader)
   {
      return shader->cfg->blocks[shader->cfg->num_blocks - 1]->end_ip + 1;
   }

   /**
    * Calculate the local ordered_address instruction counter at every
    * instruction of the shader for subsequent constant-time look-up.
    */
   ordered_address *
   ordered_inst_addresses(const fs_visitor *shader)
   {
      ordered_address *jps = new ordered_address[num_instructions(shader)];
      ordered_address jp = 0;
      unsigned ip = 0;

      foreach_block_and_inst(block, fs_inst, inst, shader->cfg) {
         jps[ip] = jp;
         jp += ordered_unit(inst);
         ip++;
      }

      return jps;
   }

   /**
    * Synchronization mode required for data manipulated by in-order
    * instructions.
    *
    * Similar to tgl_sbid_mode, but without SET mode.  Defined as a separate
    * enum for additional type safety.  The hardware doesn't provide control
    * over the synchronization mode for RegDist annotations, this is only used
    * internally in this pass in order to optimize out redundant read
    * dependencies where possible.
    */
   enum tgl_regdist_mode {
      TGL_REGDIST_NULL = 0,
      TGL_REGDIST_SRC = 1,
      TGL_REGDIST_DST = 2
   };

   /**
    * Allow bitwise arithmetic of tgl_regdist_mode enums.
    */
   tgl_regdist_mode
   operator|(tgl_regdist_mode x, tgl_regdist_mode y)
   {
      return tgl_regdist_mode(unsigned(x) | unsigned(y));
   }

   tgl_regdist_mode
   operator&(tgl_regdist_mode x, tgl_regdist_mode y)
   {
      return tgl_regdist_mode(unsigned(x) & unsigned(y));
   }

   tgl_regdist_mode &
   operator|=(tgl_regdist_mode &x, tgl_regdist_mode y)
   {
      return x = x | y;
   }

   tgl_regdist_mode &
   operator&=(tgl_regdist_mode &x, tgl_regdist_mode y)
   {
      return x = x & y;
   }

   /** @} */

   /**
    * Representation of an equivalence relation among the set of unsigned
    * integers.
    *
    * Its initial state is the identity relation '~' such that i ~ j if and
    * only if i == j for every pair of unsigned integers i and j.
    */
   struct equivalence_relation {
      equivalence_relation(unsigned n) : is(new unsigned[n]), n(n)
      {
         for (unsigned i = 0; i < n; i++)
            is[i] = i;
      }

      ~equivalence_relation()
      {
         delete[] is;
      }

      /**
       * Return equivalence class index of the specified element.  Effectively
       * this is the numeric value of an arbitrary representative from the
       * equivalence class.
       *
       * Allows the evaluation of the equivalence relation according to the
       * rule that i ~ j if and only if lookup(i) == lookup(j).
       */
      unsigned
      lookup(unsigned i) const
      {
         if (i < n && is[i] != i)
            return lookup(is[i]);
         else
            return i;
      }

      /**
       * Create an array with the results of the lookup() method for
       * constant-time evaluation.
       */
      unsigned *
      flatten() const
      {
         unsigned *ids = new unsigned[n];

         for (unsigned i = 0; i < n; i++)
            ids[i] = lookup(i);

         return ids;
      }

      /**
       * Mutate the existing equivalence relation minimally by imposing the
       * additional requirement that i ~ j.
       *
       * The algorithm updates the internal representation recursively in
       * order to guarantee transitivity while preserving the previously
       * specified equivalence requirements.
       */
      unsigned
      link(unsigned i, unsigned j)
      {
         const unsigned k = lookup(i);
         assign(i, k);
         assign(j, k);
         return k;
      }

   private:
      equivalence_relation(const equivalence_relation &);

      equivalence_relation &
      operator=(const equivalence_relation &);

      /**
       * Assign the representative of \p from to be equivalent to \p to.
       *
       * At the same time the data structure is partially flattened as much as
       * it's possible without increasing the number of recursive calls.
       */
      void
      assign(unsigned from, unsigned to)
      {
         if (from != to) {
            assert(from < n);

            if (is[from] != from)
               assign(is[from], to);

            is[from] = to;
         }
      }

      unsigned *is;
      unsigned n;
   };

   /**
    * Representation of a data dependency between two instructions in the
    * program.
    * @{
    */
   struct dependency {
      /**
       * No dependency information.
       */
      dependency() : ordered(TGL_REGDIST_NULL), jp(INT_MIN),
                     unordered(TGL_SBID_NULL), id(0),
                     exec_all(false) {}

      /**
       * Construct a dependency on the in-order instruction with the provided
       * ordered_address instruction counter.
       */
      dependency(tgl_regdist_mode mode, ordered_address jp, bool exec_all) :
         ordered(mode), jp(jp), unordered(TGL_SBID_NULL), id(0),
         exec_all(exec_all) {}

      /**
       * Construct a dependency on the out-of-order instruction with the
       * specified synchronization token.
       */
      dependency(tgl_sbid_mode mode, unsigned id, bool exec_all) :
         ordered(TGL_REGDIST_NULL), jp(INT_MIN), unordered(mode), id(id),
         exec_all(exec_all) {}

      /**
       * Synchronization mode of in-order dependency, or zero if no in-order
       * dependency is present.
       */
      tgl_regdist_mode ordered;

      /**
       * Instruction counter of in-order dependency.
       *
       * For a dependency part of a different block in the program, this is
       * relative to the specific control flow path taken between the
       * dependency and the current block: It is the ordered_address such that
       * the difference between it and the ordered_address of the first
       * instruction of the current block is exactly the number of in-order
       * instructions across that control flow path.  It is not guaranteed to
       * be equal to the local ordered_address of the generating instruction
       * [as returned by ordered_inst_addresses()], except for block-local
       * dependencies.
       */
      ordered_address jp;

      /**
       * Synchronization mode of unordered dependency, or zero if no unordered
       * dependency is present.
       */
      tgl_sbid_mode unordered;

      /** Synchronization token of out-of-order dependency. */
      unsigned id;

      /**
       * Whether the dependency could be run with execution masking disabled,
       * which might lead to the unwanted execution of the generating
       * instruction in cases where a BB is executed with all channels
       * disabled due to hardware bug GEN:BUG:1407528679.
       */
      bool exec_all;

      /**
       * Trivial in-order dependency that's always satisfied.
       *
       * Note that unlike a default-constructed dependency() which is also
       * trivially satisfied, this is considered to provide dependency
       * information and can be used to clear a previously pending dependency
       * via shadow().
       */
      static const dependency done;

      friend bool
      operator==(const dependency &dep0, const dependency &dep1)
      {
         return dep0.ordered == dep1.ordered &&
                dep0.jp == dep1.jp &&
                dep0.unordered == dep1.unordered &&
                dep0.id == dep1.id &&
                dep0.exec_all == dep1.exec_all;
      }

      friend bool
      operator!=(const dependency &dep0, const dependency &dep1)
      {
         return !(dep0 == dep1);
      }
   };

   const dependency dependency::done = dependency(TGL_REGDIST_SRC, INT_MIN, false);

   /**
    * Return whether \p dep contains any dependency information.
    */
   bool
   is_valid(const dependency &dep)
   {
      return dep.ordered || dep.unordered;
   }

   /**
    * Combine \p dep0 and \p dep1 into a single dependency object that is only
    * satisfied when both original dependencies are satisfied.  This might
    * involve updating the equivalence relation \p eq in order to make sure
    * that both out-of-order dependencies are assigned the same hardware SBID
    * as synchronization token.
    */
   dependency
   merge(equivalence_relation &eq,
         const dependency &dep0, const dependency &dep1)
   {
      dependency dep;

      if (dep0.ordered || dep1.ordered) {
         dep.ordered = dep0.ordered | dep1.ordered;
         dep.jp = MAX2(dep0.jp, dep1.jp);
      }

      if (dep0.unordered || dep1.unordered) {
         dep.unordered = dep0.unordered | dep1.unordered;
         dep.id = eq.link(dep0.unordered ? dep0.id : dep1.id,
                          dep1.unordered ? dep1.id : dep0.id);
      }

      dep.exec_all = dep0.exec_all || dep1.exec_all;

      return dep;
   }

   /**
    * Override dependency information of \p dep0 with that of \p dep1.
    */
   dependency
   shadow(const dependency &dep0, const dependency &dep1)
   {
      return is_valid(dep1) ? dep1 : dep0;
   }

   /**
    * Translate dependency information across the program.
    *
    * This returns a dependency on the same instruction translated to the
    * ordered_address space of a different block.  The correct shift for
    * transporting a dependency across an edge of the CFG is the difference
    * between the local ordered_address of the first instruction of the target
    * block and the local ordered_address of the instruction immediately after
    * the end of the origin block.
    */
   dependency
   transport(dependency dep, int delta)
   {
      if (dep.ordered && dep.jp > INT_MIN)
         dep.jp += delta;

      return dep;
   }

   /**
    * Return simplified dependency removing any synchronization modes not
    * applicable to an instruction reading the same register location.
    */
   dependency
   dependency_for_read(dependency dep)
   {
      dep.ordered &= TGL_REGDIST_DST;
      return dep;
   }

   /**
    * Return simplified dependency removing any synchronization modes not
    * applicable to an instruction \p inst writing the same register location.
    */
   dependency
   dependency_for_write(const fs_inst *inst, dependency dep)
   {
      if (!is_unordered(inst))
         dep.ordered &= TGL_REGDIST_DST;
      return dep;
   }

   /** @} */

   /**
    * Scoreboard representation.  This keeps track of the data dependencies of
    * registers with GRF granularity.
    */
   class scoreboard {
   public:
      /**
       * Look up the most current data dependency for register \p r.
       */
      dependency
      get(const fs_reg &r) const
      {
         if (const dependency *p = const_cast<scoreboard *>(this)->dep(r))
            return *p;
         else
            return dependency();
      }

      /**
       * Specify the most current data dependency for register \p r.
       */
      void
      set(const fs_reg &r, const dependency &d)
      {
         if (dependency *p = dep(r))
            *p = d;
      }

      /**
       * Component-wise merge() of corresponding dependencies from two
       * scoreboard objects.  \sa merge().
       */
      friend scoreboard
      merge(equivalence_relation &eq,
            const scoreboard &sb0, const scoreboard &sb1)
      {
         scoreboard sb;

         for (unsigned i = 0; i < ARRAY_SIZE(sb.grf_deps); i++)
            sb.grf_deps[i] = merge(eq, sb0.grf_deps[i], sb1.grf_deps[i]);

         sb.addr_dep = merge(eq, sb0.addr_dep, sb1.addr_dep);

         for (unsigned i = 0; i < ARRAY_SIZE(sb.accum_deps); i++)
            sb.accum_deps[i] = merge(eq, sb0.accum_deps[i], sb1.accum_deps[i]);

         return sb;
      }

      /**
       * Component-wise shadow() of corresponding dependencies from two
       * scoreboard objects.  \sa shadow().
       */
      friend scoreboard
      shadow(const scoreboard &sb0, const scoreboard &sb1)
      {
         scoreboard sb;

         for (unsigned i = 0; i < ARRAY_SIZE(sb.grf_deps); i++)
            sb.grf_deps[i] = shadow(sb0.grf_deps[i], sb1.grf_deps[i]);

         sb.addr_dep = shadow(sb0.addr_dep, sb1.addr_dep);

         for (unsigned i = 0; i < ARRAY_SIZE(sb.accum_deps); i++)
            sb.accum_deps[i] = shadow(sb0.accum_deps[i], sb1.accum_deps[i]);

         return sb;
      }

      /**
       * Component-wise transport() of dependencies from a scoreboard
       * object.  \sa transport().
       */
      friend scoreboard
      transport(const scoreboard &sb0, int delta)
      {
         scoreboard sb;

         for (unsigned i = 0; i < ARRAY_SIZE(sb.grf_deps); i++)
            sb.grf_deps[i] = transport(sb0.grf_deps[i], delta);

         sb.addr_dep = transport(sb0.addr_dep, delta);

         for (unsigned i = 0; i < ARRAY_SIZE(sb.accum_deps); i++)
            sb.accum_deps[i] = transport(sb0.accum_deps[i], delta);

         return sb;
      }

      friend bool
      operator==(const scoreboard &sb0, const scoreboard &sb1)
      {
         for (unsigned i = 0; i < ARRAY_SIZE(sb0.grf_deps); i++) {
            if (sb0.grf_deps[i] != sb1.grf_deps[i])
               return false;
         }

         if (sb0.addr_dep != sb1.addr_dep)
            return false;

         for (unsigned i = 0; i < ARRAY_SIZE(sb0.accum_deps); i++) {
            if (sb0.accum_deps[i] != sb1.accum_deps[i])
               return false;
         }

         return true;
      }

      friend bool
      operator!=(const scoreboard &sb0, const scoreboard &sb1)
      {
         return !(sb0 == sb1);
      }

   private:
      dependency grf_deps[BRW_MAX_GRF];
      dependency addr_dep;
      dependency accum_deps[10];

      dependency *
      dep(const fs_reg &r)
      {
         const unsigned reg = (r.file == VGRF ? r.nr + r.offset / REG_SIZE :
                               reg_offset(r) / REG_SIZE);

         return (r.file == VGRF || r.file == FIXED_GRF ? &grf_deps[reg] :
                 r.file == MRF ? &grf_deps[GEN7_MRF_HACK_START + reg] :
                 r.file == ARF && reg >= BRW_ARF_ADDRESS &&
                                  reg < BRW_ARF_ACCUMULATOR ? &addr_dep :
                 r.file == ARF && reg >= BRW_ARF_ACCUMULATOR &&
                                  reg < BRW_ARF_FLAG ? &accum_deps[
                                     reg - BRW_ARF_ACCUMULATOR] :
                 NULL);
      }
   };

   /**
    * Dependency list handling.
    * @{
    */
   struct dependency_list {
      dependency_list() : deps(NULL), n(0) {}

      ~dependency_list()
      {
         free(deps);
      }

      void
      push_back(const dependency &dep)
      {
         deps = (dependency *)realloc(deps, (n + 1) * sizeof(*deps));
         deps[n++] = dep;
      }

      unsigned
      size() const
      {
         return n;
      }

      const dependency &
      operator[](unsigned i) const
      {
         assert(i < n);
         return deps[i];
      }

      dependency &
      operator[](unsigned i)
      {
         assert(i < n);
         return deps[i];
      }

   private:
      dependency_list(const dependency_list &);
      dependency_list &
      operator=(const dependency_list &);

      dependency *deps;
      unsigned n;
   };

   /**
    * Add dependency \p dep to the list of dependencies of an instruction
    * \p deps.
    */
   void
   add_dependency(const unsigned *ids, dependency_list &deps, dependency dep)
   {
      if (is_valid(dep)) {
         /* Translate the unordered dependency token first in order to keep
          * the list minimally redundant.
          */
         if (dep.unordered)
            dep.id = ids[dep.id];

         /* Try to combine the specified dependency with any existing ones. */
         for (unsigned i = 0; i < deps.size(); i++) {
            /* Don't combine otherwise matching dependencies if there is an
             * exec_all mismatch which would cause a SET dependency to gain an
             * exec_all flag, since that would prevent it from being baked
             * into the instruction we want to allocate an SBID for.
             */
            if (deps[i].exec_all != dep.exec_all &&
                (!deps[i].exec_all || (dep.unordered & TGL_SBID_SET)) &&
                (!dep.exec_all || (deps[i].unordered & TGL_SBID_SET)))
               continue;

            if (dep.ordered && deps[i].ordered) {
               deps[i].jp = MAX2(deps[i].jp, dep.jp);
               deps[i].ordered |= dep.ordered;
               deps[i].exec_all |= dep.exec_all;
               dep.ordered = TGL_REGDIST_NULL;
            }

            if (dep.unordered && deps[i].unordered && deps[i].id == dep.id) {
               deps[i].unordered |= dep.unordered;
               deps[i].exec_all |= dep.exec_all;
               dep.unordered = TGL_SBID_NULL;
            }
         }

         /* Add it to the end of the list if necessary. */
         if (is_valid(dep))
            deps.push_back(dep);
      }
   }

   /**
    * Construct a tgl_swsb annotation encoding any ordered dependencies from
    * the dependency list \p deps of an instruction with ordered_address \p
    * jp.  If \p exec_all is false only dependencies known to be executed with
    * channel masking applied will be considered in the calculation.
    */
   tgl_swsb
   ordered_dependency_swsb(const dependency_list &deps,
                           const ordered_address &jp,
                           bool exec_all)
   {
      unsigned min_dist = ~0u;

      for (unsigned i = 0; i < deps.size(); i++) {
         if (deps[i].ordered && exec_all >= deps[i].exec_all) {
            const unsigned dist = jp - deps[i].jp;
            const unsigned max_dist = 10;
            assert(jp > deps[i].jp);
            if (dist <= max_dist)
               min_dist = MIN3(min_dist, dist, 7);
         }
      }

      return { min_dist == ~0u ? 0 : min_dist };
   }

   /**
    * Return whether the dependency list \p deps of an instruction with
    * ordered_address \p jp has any non-trivial ordered dependencies.  If \p
    * exec_all is false only dependencies known to be executed with channel
    * masking applied will be considered in the calculation.
    */
   bool
   find_ordered_dependency(const dependency_list &deps,
                           const ordered_address &jp,
                           bool exec_all)
   {
      return ordered_dependency_swsb(deps, jp, exec_all).regdist;
   }

   /**
    * Return the full tgl_sbid_mode bitset for the first unordered dependency
    * on the list \p deps that matches the specified tgl_sbid_mode, or zero if
    * no such dependency is present.  If \p exec_all is false only
    * dependencies known to be executed with channel masking applied will be
    * considered in the calculation.
    */
   tgl_sbid_mode
   find_unordered_dependency(const dependency_list &deps,
                             tgl_sbid_mode unordered,
                             bool exec_all)
   {
      if (unordered) {
         for (unsigned i = 0; i < deps.size(); i++) {
            if ((unordered & deps[i].unordered) &&
                exec_all >= deps[i].exec_all)
               return deps[i].unordered;
         }
      }

      return TGL_SBID_NULL;
   }

   /**
    * Return the tgl_sbid_mode bitset of an unordered dependency from the list
    * \p deps that can be represented directly in the SWSB annotation of the
    * instruction without additional SYNC instructions, or zero if no such
    * dependency is present.
    */
   tgl_sbid_mode
   baked_unordered_dependency_mode(const fs_inst *inst,
                                   const dependency_list &deps,
                                   const ordered_address &jp)
   {
      const bool exec_all = inst->force_writemask_all;
      const bool has_ordered = find_ordered_dependency(deps, jp, exec_all);

      if (find_unordered_dependency(deps, TGL_SBID_SET, exec_all))
         return find_unordered_dependency(deps, TGL_SBID_SET, exec_all);
      else if (has_ordered && is_unordered(inst))
         return TGL_SBID_NULL;
      else if (find_unordered_dependency(deps, TGL_SBID_DST, exec_all) &&
               (!has_ordered || !is_unordered(inst)))
         return find_unordered_dependency(deps, TGL_SBID_DST, exec_all);
      else if (!has_ordered)
         return find_unordered_dependency(deps, TGL_SBID_SRC, exec_all);
      else
         return TGL_SBID_NULL;
   }

   /** @} */

   /**
    * Shader instruction dependency calculation.
    * @{
    */

   /**
    * Update scoreboard object \p sb to account for the execution of
    * instruction \p inst.
    */
   void
   update_inst_scoreboard(const ordered_address *jps,
                          const fs_inst *inst, unsigned ip, scoreboard &sb)
   {
      const bool exec_all = inst->force_writemask_all;

      /* Track any source registers that may be fetched asynchronously by this
       * instruction, otherwise clear the dependency in order to avoid
       * subsequent redundant synchronization.
       */
      for (unsigned i = 0; i < inst->sources; i++) {
         const dependency rd_dep =
            (inst->is_payload(i) ||
             inst->is_math()) ? dependency(TGL_SBID_SRC, ip, exec_all) :
            ordered_unit(inst) ? dependency(TGL_REGDIST_SRC, jps[ip], exec_all) :
            dependency::done;

         for (unsigned j = 0; j < regs_read(inst, i); j++)
            sb.set(byte_offset(inst->src[i], REG_SIZE * j), rd_dep);
      }

      if (is_send(inst) && inst->base_mrf != -1) {
         const dependency rd_dep = dependency(TGL_SBID_SRC, ip, exec_all);

         for (unsigned j = 0; j < inst->mlen; j++)
            sb.set(brw_uvec_mrf(8, inst->base_mrf + j, 0), rd_dep);
      }

      /* Track any destination registers of this instruction. */
      const dependency wr_dep =
         is_unordered(inst) ? dependency(TGL_SBID_DST, ip, exec_all) :
         ordered_unit(inst) ? dependency(TGL_REGDIST_DST, jps[ip], exec_all) :
         dependency();

      if (is_valid(wr_dep) && inst->dst.file != BAD_FILE &&
          !inst->dst.is_null()) {
         for (unsigned j = 0; j < regs_written(inst); j++)
            sb.set(byte_offset(inst->dst, REG_SIZE * j), wr_dep);
      }
   }

   /**
    * Calculate scoreboard objects locally that represent any pending (and
    * unconditionally resolved) dependencies at the end of each block of the
    * program.
    */
   scoreboard *
   gather_block_scoreboards(const fs_visitor *shader,
                            const ordered_address *jps)
   {
      scoreboard *sbs = new scoreboard[shader->cfg->num_blocks];
      unsigned ip = 0;

      foreach_block_and_inst(block, fs_inst, inst, shader->cfg)
         update_inst_scoreboard(jps, inst, ip++, sbs[block->num]);

      return sbs;
   }

   /**
    * Propagate data dependencies globally through the control flow graph
    * until a fixed point is reached.
    *
    * Calculates the set of dependencies potentially pending at the beginning
    * of each block, and returns it as an array of scoreboard objects.
    */
   scoreboard *
   propagate_block_scoreboards(const fs_visitor *shader,
                               const ordered_address *jps,
                               equivalence_relation &eq)
   {
      const scoreboard *delta_sbs = gather_block_scoreboards(shader, jps);
      scoreboard *in_sbs = new scoreboard[shader->cfg->num_blocks];
      scoreboard *out_sbs = new scoreboard[shader->cfg->num_blocks];

      for (bool progress = true; progress;) {
         progress = false;

         foreach_block(block, shader->cfg) {
            const scoreboard sb = shadow(in_sbs[block->num],
                                         delta_sbs[block->num]);

            if (sb != out_sbs[block->num]) {
               foreach_list_typed(bblock_link, child_link, link,
                                  &block->children) {
                  scoreboard &in_sb = in_sbs[child_link->block->num];
                  const int delta =
                     jps[child_link->block->start_ip] - jps[block->end_ip]
                     - ordered_unit(static_cast<const fs_inst *>(block->end()));

                  in_sb = merge(eq, in_sb, transport(sb, delta));
               }

               out_sbs[block->num] = sb;
               progress = true;
            }
         }
      }

      delete[] delta_sbs;
      delete[] out_sbs;

      return in_sbs;
   }

   /**
    * Return the list of potential dependencies of each instruction in the
    * shader based on the result of global dependency analysis.
    */
   dependency_list *
   gather_inst_dependencies(const fs_visitor *shader,
                            const ordered_address *jps)
   {
      equivalence_relation eq(num_instructions(shader));
      scoreboard *sbs = propagate_block_scoreboards(shader, jps, eq);
      const unsigned *ids = eq.flatten();
      dependency_list *deps = new dependency_list[num_instructions(shader)];
      unsigned ip = 0;

      foreach_block_and_inst(block, fs_inst, inst, shader->cfg) {
         const bool exec_all = inst->force_writemask_all;
         scoreboard &sb = sbs[block->num];

         for (unsigned i = 0; i < inst->sources; i++) {
            for (unsigned j = 0; j < regs_read(inst, i); j++)
               add_dependency(ids, deps[ip], dependency_for_read(
                  sb.get(byte_offset(inst->src[i], REG_SIZE * j))));
         }

         if (is_send(inst) && inst->base_mrf != -1) {
            for (unsigned j = 0; j < inst->mlen; j++)
               add_dependency(ids, deps[ip], dependency_for_read(
                  sb.get(brw_uvec_mrf(8, inst->base_mrf + j, 0))));
         }

         if (is_unordered(inst))
            add_dependency(ids, deps[ip],
                           dependency(TGL_SBID_SET, ip, exec_all));

         if (!inst->no_dd_check) {
            if (inst->dst.file != BAD_FILE && !inst->dst.is_null()) {
               for (unsigned j = 0; j < regs_written(inst); j++) {
                  add_dependency(ids, deps[ip], dependency_for_write(inst,
                     sb.get(byte_offset(inst->dst, REG_SIZE * j))));
               }
            }

            if (is_send(inst) && inst->base_mrf != -1) {
               for (unsigned j = 0; j < inst->implied_mrf_writes(); j++)
                  add_dependency(ids, deps[ip], dependency_for_write(inst,
                     sb.get(brw_uvec_mrf(8, inst->base_mrf + j, 0))));
            }
         }

         update_inst_scoreboard(jps, inst, ip, sb);
         ip++;
      }

      delete[] sbs;
      delete[] ids;

      return deps;
   }

   /** @} */

   /**
    * Allocate SBID tokens to track the execution of every out-of-order
    * instruction of the shader.
    */
   dependency_list *
   allocate_inst_dependencies(const fs_visitor *shader,
                              const dependency_list *deps0)
   {
      /* XXX - Use bin-packing algorithm to assign hardware SBIDs optimally in
       *       shaders with a large number of SEND messages.
       */

      /* Allocate an unordered dependency ID to hardware SBID translation
       * table with as many entries as instructions there are in the shader,
       * which is the maximum number of unordered IDs we can find in the
       * program.
       */
      unsigned *ids = new unsigned[num_instructions(shader)];
      for (unsigned ip = 0; ip < num_instructions(shader); ip++)
         ids[ip] = ~0u;

      dependency_list *deps1 = new dependency_list[num_instructions(shader)];
      unsigned next_id = 0;

      for (unsigned ip = 0; ip < num_instructions(shader); ip++) {
         for (unsigned i = 0; i < deps0[ip].size(); i++) {
            const dependency &dep = deps0[ip][i];

            if (dep.unordered && ids[dep.id] == ~0u)
               ids[dep.id] = (next_id++) & 0xf;

            add_dependency(ids, deps1[ip], dep);
         }
      }

      delete[] ids;

      return deps1;
   }

   /**
    * Emit dependency information provided by \p deps into the shader,
    * inserting additional SYNC instructions for dependencies that can't be
    * represented directly by annotating existing instructions.
    */
   void
   emit_inst_dependencies(fs_visitor *shader,
                          const ordered_address *jps,
                          const dependency_list *deps)
   {
      unsigned ip = 0;

      foreach_block_and_inst_safe(block, fs_inst, inst, shader->cfg) {
         const bool exec_all = inst->force_writemask_all;
         tgl_swsb swsb = ordered_dependency_swsb(deps[ip], jps[ip], exec_all);
         const tgl_sbid_mode unordered_mode =
            baked_unordered_dependency_mode(inst, deps[ip], jps[ip]);

         for (unsigned i = 0; i < deps[ip].size(); i++) {
            const dependency &dep = deps[ip][i];

            if (dep.unordered) {
               if (unordered_mode == dep.unordered &&
                   exec_all >= dep.exec_all && !swsb.mode) {
                  /* Bake unordered dependency into the instruction's SWSB if
                   * possible, except in cases where the current instruction
                   * isn't marked NoMask but the dependency is, since that
                   * might lead to data coherency issues due to
                   * GEN:BUG:1407528679.
                   */
                  swsb.sbid = dep.id;
                  swsb.mode = dep.unordered;
               } else {
                  /* Emit dependency into the SWSB of an extra SYNC
                   * instruction.
                   */
                  const fs_builder ibld = fs_builder(shader, block, inst)
                                          .exec_all().group(1, 0);
                  fs_inst *sync = ibld.emit(BRW_OPCODE_SYNC, ibld.null_reg_ud(),
                                            brw_imm_ud(TGL_SYNC_NOP));
                  sync->sched.sbid = dep.id;
                  sync->sched.mode = dep.unordered;
                  assert(!(sync->sched.mode & TGL_SBID_SET));
               }
            }
         }

         for (unsigned i = 0; i < deps[ip].size(); i++) {
            const dependency &dep = deps[ip][i];

            if (dep.ordered && dep.exec_all > exec_all &&
                find_ordered_dependency(deps[ip], jps[ip], true)) {
               /* If the current instruction is not marked NoMask but an
                * ordered dependency is, perform the synchronization as a
                * separate NoMask SYNC instruction in order to avoid data
                * coherency issues due to GEN:BUG:1407528679.  The similar
                * scenario with unordered dependencies should have been
                * handled above.
                */
               const fs_builder ibld = fs_builder(shader, block, inst)
                                       .exec_all().group(1, 0);
               fs_inst *sync = ibld.emit(BRW_OPCODE_SYNC, ibld.null_reg_ud(),
                                         brw_imm_ud(TGL_SYNC_NOP));
               sync->sched = ordered_dependency_swsb(deps[ip], jps[ip], true);
               break;
            }
         }

         /* Update the IR. */
         inst->sched = swsb;
         inst->no_dd_check = inst->no_dd_clear = false;
         ip++;
      }
   }
}

bool
fs_visitor::lower_scoreboard()
{
   if (devinfo->gen >= 12) {
      const ordered_address *jps = ordered_inst_addresses(this);
      const dependency_list *deps0 = gather_inst_dependencies(this, jps);
      const dependency_list *deps1 = allocate_inst_dependencies(this, deps0);
      emit_inst_dependencies(this, jps, deps1);
      delete[] deps1;
      delete[] deps0;
      delete[] jps;
   }

   return true;
}
