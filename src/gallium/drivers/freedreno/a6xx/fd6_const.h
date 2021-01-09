/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#ifndef FD6_CONST_H
#define FD6_CONST_H

#include "fd6_emit.h"

void fd6_emit_consts(struct fd6_emit *emit);
void fd6_emit_ibo_consts(struct fd6_emit *emit, const struct ir3_shader_variant *v,
		enum pipe_shader_type stage, struct fd_ringbuffer *ring);
void fd6_emit_cs_consts(const struct ir3_shader_variant *v, struct fd_ringbuffer *ring,
		struct fd_context *ctx, const struct pipe_grid_info *info);
void fd6_emit_immediates(struct fd_screen *screen, const struct ir3_shader_variant *v,
		struct fd_ringbuffer *ring);
void fd6_emit_link_map(struct fd_screen *screen,
		const struct ir3_shader_variant *producer,
		const struct ir3_shader_variant *v, struct fd_ringbuffer *ring);

#endif /* FD6_CONST_H */
