# UAC 1.4.0/1.5.0 dereferences a null/freed interface on its open failure path.
# Generate a corrected translation unit without modifying managed components.
idf_component_get_property(uac_lib espressif__usb_host_uac COMPONENT_LIB)
idf_component_get_property(uac_dir espressif__usb_host_uac COMPONENT_DIR)
file(READ "${uac_dir}/uac_host.c" uac_source)
set(old_cleanup [=[fail:
    if (uac_iface) {
        uac_host_interface_delete(uac_iface);
    }
    if (new_device) {
        _uac_host_device_delete(uac_device);
    }
    if (dev_hdl) {
        usb_host_device_close(s_uac_driver->client_handle, dev_hdl);
    }
    if (uac_iface->ringbuf) {
        vRingbufferDelete(uac_iface->ringbuf);
    }
    return ret;]=])
set(new_cleanup [=[fail:
    if (uac_iface) {
        if (uac_iface->ringbuf) {
            vRingbufferDelete(uac_iface->ringbuf);
            uac_iface->ringbuf = NULL;
        }
        uac_host_interface_delete(uac_iface);
    }
    if (new_device) {
        _uac_host_device_delete(uac_device);
    }
    if (dev_hdl) {
        usb_host_device_close(s_uac_driver->client_handle, dev_hdl);
    }
    return ret;]=])
string(FIND "${uac_source}" "${old_cleanup}" cleanup_position)
if(cleanup_position LESS 0)
    message(FATAL_ERROR "UAC source changed; review the P4 open-failure cleanup fix")
endif()
string(REPLACE "${old_cleanup}" "${new_cleanup}" uac_source "${uac_source}")
set(patched_uac "${CMAKE_CURRENT_BINARY_DIR}/uac_host_cleanup_fixed.c")
file(CONFIGURE OUTPUT "${patched_uac}" CONTENT "${uac_source}" @ONLY)
get_target_property(uac_sources ${uac_lib} SOURCES)
list(FILTER uac_sources EXCLUDE REGEX "(^|/)uac_host\\.c$")
set_property(TARGET ${uac_lib} PROPERTY SOURCES "${uac_sources};${patched_uac}")
target_include_directories(${uac_lib} PRIVATE "${uac_dir}")
