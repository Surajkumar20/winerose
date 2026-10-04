# Enforces the layer rules in PLAN.md §1.1 at configure time.
#
#   winerose_forbid_links(<target> <regex>...)
#       Fails if <target>'s own link libraries, or anything they expose to it transitively
#       (INTERFACE_LINK_LIBRARIES, excluding link-only $<LINK_ONLY:...> entries), matches a regex.
#
#   winerose_forbid_includes(<dir> <regex>...)
#       Fails if any .h/.hpp/.ipp/.cpp under <dir> has an #include line matching a regex.
#       Catches what link rules can't, e.g. JUCE UI sources compiled inside the plugin target.
#
# Rules are recorded when declared and evaluated by winerose_check_layering(), called once at the end
# of the root CMakeLists.txt after every target exists.

function(winerose_forbid_links target)
    set_property(GLOBAL APPEND PROPERTY WINEROSE_LINK_RULE_TARGETS ${target})
    set_property(GLOBAL PROPERTY WINEROSE_LINK_RULE_${target} "${ARGN}")
endfunction()

function(winerose_forbid_includes dir)
    set_property(GLOBAL APPEND PROPERTY WINEROSE_INCLUDE_RULE_DIRS "${dir}")
    string(MD5 id "${dir}")
    set_property(GLOBAL PROPERTY WINEROSE_INCLUDE_RULE_${id} "${ARGN}")
endfunction()

# Collect every target/library name reachable from <target> as a usage requirement.
function(_winerose_reachable target out_var)
    set(seen "")
    get_target_property(root_type ${target} TYPE)
    if(root_type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(stack ${target} INTERFACE_LINK_LIBRARIES)
    else()
        get_target_property(stack ${target} LINK_LIBRARIES)
        get_target_property(iface ${target} INTERFACE_LINK_LIBRARIES)
        if(iface)
            list(APPEND stack ${iface})
        endif()
    endif()
    if(NOT stack)
        set(stack "")
    endif()

    while(stack)
        list(POP_BACK stack cur)
        if(cur MATCHES "^\\$<LINK_ONLY:" OR cur MATCHES "^\\$<" OR cur STREQUAL "")
            continue()
        endif()
        if(cur IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${cur})
        if(TARGET ${cur})
            get_target_property(real ${cur} ALIASED_TARGET)
            if(real)
                list(APPEND seen ${real})
                set(cur ${real})
            endif()
            get_target_property(next ${cur} INTERFACE_LINK_LIBRARIES)
            if(next)
                list(APPEND stack ${next})
            endif()
        endif()
    endwhile()
    set(${out_var} "${seen}" PARENT_SCOPE)
endfunction()

function(winerose_check_layering)
    get_property(targets GLOBAL PROPERTY WINEROSE_LINK_RULE_TARGETS)
    foreach(t IN LISTS targets)
        get_property(rules GLOBAL PROPERTY WINEROSE_LINK_RULE_${t})
        _winerose_reachable(${t} deps)
        foreach(dep IN LISTS deps)
            foreach(rule IN LISTS rules)
                if(dep MATCHES "${rule}")
                    message(FATAL_ERROR
                        "Layering violation: '${t}' reaches '${dep}' (forbidden by '${rule}'). See PLAN.md §1.1.")
                endif()
            endforeach()
        endforeach()
    endforeach()

    get_property(dirs GLOBAL PROPERTY WINEROSE_INCLUDE_RULE_DIRS)
    foreach(dir IN LISTS dirs)
        string(MD5 id "${dir}")
        get_property(rules GLOBAL PROPERTY WINEROSE_INCLUDE_RULE_${id})
        file(GLOB_RECURSE files "${dir}/*.h" "${dir}/*.hpp" "${dir}/*.ipp" "${dir}/*.cpp")
        foreach(f IN LISTS files)
            file(STRINGS "${f}" includes REGEX "^[ \t]*#[ \t]*include")
            foreach(line IN LISTS includes)
                foreach(rule IN LISTS rules)
                    if(line MATCHES "${rule}")
                        message(FATAL_ERROR
                            "Layering violation: ${f} has '${line}' (forbidden by '${rule}'). See PLAN.md §1.1.")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endforeach()
    message(STATUS "Winerose layering check passed")
endfunction()
