COPYRIGHT = """\
/*
 * Copyright 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import os.path
import re
import sys

VULKAN_UTIL = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../vulkan/util'))
sys.path.append(VULKAN_UTIL)

from vk_extensions import *
from vk_extensions_gen import *

MAX_API_VERSION = '1.1.107'

# Supported API versions.  Each one is the maximum patch version for the given
# version.  Version come in increasing order and each version is available if
# it's provided "enable" condition is true and all previous versions are
# available.
# TODO: The patch version should be unified!
API_VERSIONS = [
    ApiVersion('1.0.68',  True),
    ApiVersion('1.1.107', False),
    ApiVersion('1.2.131', False),
]

MAX_API_VERSION = None # Computed later

# On Android, we disable all surface and swapchain extensions. Android's Vulkan
# loader implements VK_KHR_surface and VK_KHR_swapchain, and applications
# cannot access the driver's implementation. Moreoever, if the driver exposes
# the those extension strings, then tests dEQP-VK.api.info.instance.extensions
# and dEQP-VK.api.info.device fail due to the duplicated strings.
EXTENSIONS = [
    Extension('VK_ANDROID_native_buffer',                 5, False),
    Extension('VK_KHR_16bit_storage',                     1, False),
    Extension('VK_KHR_bind_memory2',                      1, True),
    Extension('VK_KHR_create_renderpass2',                1, False),
    Extension('VK_KHR_dedicated_allocation',              1, True),
    Extension('VK_KHR_depth_stencil_resolve',             1, False),
    Extension('VK_KHR_descriptor_update_template',        1, True),
    Extension('VK_KHR_device_group',                      1, True),
    Extension('VK_KHR_device_group_creation',             1, True),
    Extension('VK_KHR_draw_indirect_count',               1, True),
    Extension('VK_KHR_driver_properties',                 1, True),
    Extension('VK_KHR_external_fence',                    1, False),
    Extension('VK_KHR_external_fence_capabilities',       1, True),
    Extension('VK_KHR_external_fence_fd',                 1, False),
    Extension('VK_KHR_external_memory',                   1, False),
    Extension('VK_KHR_external_memory_capabilities',      1, True),
    Extension('VK_KHR_external_memory_fd',                1, False),
    Extension('VK_KHR_external_semaphore',                1, False),
    Extension('VK_KHR_external_semaphore_capabilities',   1, True),
    Extension('VK_KHR_external_semaphore_fd',             1, False),
    Extension('VK_KHR_get_display_properties2',           1, 'VK_USE_PLATFORM_DISPLAY_KHR'),
    Extension('VK_KHR_get_memory_requirements2',          1, True),
    Extension('VK_KHR_get_physical_device_properties2',   1, True),
    Extension('VK_KHR_get_surface_capabilities2',         1, 'LVP_HAS_SURFACE'),
    Extension('VK_KHR_image_format_list',                 1, False),
    Extension('VK_KHR_imageless_framebuffer',             1, False),
    Extension('VK_KHR_incremental_present',               1, 'LVP_HAS_SURFACE'),
    Extension('VK_KHR_maintenance1',                      1, True),
    Extension('VK_KHR_maintenance2',                      1, False),
    Extension('VK_KHR_maintenance3',                      1, False),
    Extension('VK_KHR_pipeline_executable_properties',    1, False),
    Extension('VK_KHR_push_descriptor',                   1, True),
    Extension('VK_KHR_relaxed_block_layout',              1, True),
    Extension('VK_KHR_sampler_mirror_clamp_to_edge',      1, True),
    Extension('VK_KHR_sampler_ycbcr_conversion',          1, False),
    Extension('VK_KHR_shader_atomic_int64',               1, False),
    Extension('VK_KHR_shader_draw_parameters',            1, False),
    Extension('VK_KHR_shader_float16_int8',               1, False),
    Extension('VK_KHR_storage_buffer_storage_class',      1, True),
    Extension('VK_KHR_surface',                          25, 'LVP_HAS_SURFACE'),
    Extension('VK_KHR_surface_protected_capabilities',    1, 'LVP_HAS_SURFACE'),
    Extension('VK_KHR_swapchain',                        68, 'LVP_HAS_SURFACE'),
    Extension('VK_KHR_uniform_buffer_standard_layout',    1, False),
    Extension('VK_KHR_variable_pointers',                 1, False),
    Extension('VK_KHR_wayland_surface',                   6, 'VK_USE_PLATFORM_WAYLAND_KHR'),
    Extension('VK_KHR_xcb_surface',                       6, 'VK_USE_PLATFORM_XCB_KHR'),
    Extension('VK_KHR_xlib_surface',                      6, 'VK_USE_PLATFORM_XLIB_KHR'),
    Extension('VK_KHR_multiview',                         1, False),
    Extension('VK_KHR_display',                          23, 'VK_USE_PLATFORM_DISPLAY_KHR'),
    Extension('VK_KHR_8bit_storage',                      1, False),
    Extension('VK_EXT_direct_mode_display',               1, 'VK_USE_PLATFORM_DISPLAY_KHR'),
    Extension('VK_EXT_acquire_xlib_display',              1, 'VK_USE_PLATFORM_XLIB_XRANDR_EXT'),
    Extension('VK_EXT_buffer_device_address',             1, False),
    Extension('VK_EXT_calibrated_timestamps',             1, False),
    Extension('VK_EXT_conditional_rendering',             1, False),
    Extension('VK_EXT_conservative_rasterization',        1, False),
    Extension('VK_EXT_display_surface_counter',           1, 'VK_USE_PLATFORM_DISPLAY_KHR'),
    Extension('VK_EXT_display_control',                   1, 'VK_USE_PLATFORM_DISPLAY_KHR'),
    Extension('VK_EXT_debug_report',                      9, True),
    Extension('VK_EXT_depth_clip_enable',                 1, False),
    Extension('VK_EXT_depth_range_unrestricted',          1, False),
    Extension('VK_EXT_descriptor_indexing',               2, False),
    Extension('VK_EXT_discard_rectangles',                1, False),
    Extension('VK_EXT_external_memory_dma_buf',           1, True),
    Extension('VK_EXT_external_memory_host',              1, False),
    Extension('VK_EXT_global_priority',                   1, False),
    Extension('VK_EXT_host_query_reset',                  1, False),
    Extension('VK_EXT_index_type_uint8',                  1, True),
    Extension('VK_EXT_inline_uniform_block',              1, False),
    Extension('VK_EXT_memory_budget',                     1, False),
    Extension('VK_EXT_memory_priority',                   1, False),
    Extension('VK_EXT_pci_bus_info',                      2, False),
    Extension('VK_EXT_pipeline_creation_feedback',        1, False),
    Extension('VK_EXT_post_depth_coverage',               1, True),
    Extension('VK_EXT_private_data',                      1, True),
    Extension('VK_EXT_queue_family_foreign',              1, False),
    Extension('VK_EXT_sample_locations',                  1, False),
    Extension('VK_EXT_sampler_filter_minmax',             1, False),
    Extension('VK_EXT_scalar_block_layout',               1, False),
    Extension('VK_EXT_shader_viewport_index_layer',       1, False),
    Extension('VK_EXT_shader_stencil_export',             1, True),
    Extension('VK_EXT_shader_subgroup_ballot',            1, False),
    Extension('VK_EXT_shader_subgroup_vote',              1, False),
    Extension('VK_EXT_transform_feedback',                1, True),
    Extension('VK_EXT_vertex_attribute_divisor',          3, True),
    Extension('VK_EXT_ycbcr_image_arrays',                1, False),
    Extension('VK_GOOGLE_decorate_string',                1, True),
    Extension('VK_GOOGLE_hlsl_functionality1',            1, True),
]

MAX_API_VERSION = VkVersion('0.0.0')
for version in API_VERSIONS:
    version.version = VkVersion(version.version)
    assert version.version > MAX_API_VERSION
    MAX_API_VERSION = version.version

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-c', help='Output C file.', required=True)
    parser.add_argument('--out-h', help='Output H file.', required=True)
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True,
                        action='append',
                        dest='xml_files')
    args = parser.parse_args()

    gen_extensions('lvp', args.xml_files, API_VERSIONS, MAX_API_VERSION, EXTENSIONS, args.out_c, args.out_h)
