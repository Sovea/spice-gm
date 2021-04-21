dnl
dnl Enable all known GCC compiler warnings, except for those
dnl we can't yet cope with
dnl
AC_DEFUN([SPICE_COMPILE_WARNINGS],[
    dnl ******************************
    dnl More compiler warnings
    dnl ******************************

    AC_ARG_ENABLE([werror],
                  AS_HELP_STRING([--enable-werror], [Use -Werror (if supported)]),
                  [set_werror="$enableval"],
                  [set_werror=no])

    # List of warnings that are not relevant / wanted

    # Don't care about C++ compiler compat
    dontwarn="$dontwarn -Wc++-compat"
    dontwarn="$dontwarn -Wabi"
    dontwarn="$dontwarn -Wdeprecated"
    # For older gcc versions, -Wenum-compare is "C++ and Objective-C++ only"
    # For newer gcc versions, -Wenum-compare is "enabled by -Wall"
    dontwarn="$dontwarn -Wenum-compare"
    # Don't care about ancient C standard compat
    dontwarn="$dontwarn -Wtraditional"
    # Don't care about ancient C standard compat
    dontwarn="$dontwarn -Wtraditional-conversion"
    # Ignore warnings in /usr/include
    dontwarn="$dontwarn -Wsystem-headers"
    # Happy for compiler to add struct padding
    dontwarn="$dontwarn -Wpadded"
    # GCC very confused with -O2
    dontwarn="$dontwarn -Wunreachable-code"


    dontwarn="$dontwarn -Wconversion"
    dontwarn="$dontwarn -Wsign-conversion"
    dontwarn="$dontwarn -Wvla"
    dontwarn="$dontwarn -Wundef"
    dontwarn="$dontwarn -Wcast-qual"
    dontwarn="$dontwarn -Wlong-long"
    dontwarn="$dontwarn -Wswitch-default"
    dontwarn="$dontwarn -Wswitch-enum"
    dontwarn="$dontwarn -Wstrict-overflow"
    dontwarn="$dontwarn -Wunsafe-loop-optimizations"
    dontwarn="$dontwarn -Wformat-nonliteral"
    dontwarn="$dontwarn -Wfloat-equal"
    dontwarn="$dontwarn -Wdeclaration-after-statement"
    dontwarn="$dontwarn -Wpacked"
    dontwarn="$dontwarn -Wunused-macros"
    dontwarn="$dontwarn -Woverlength-strings"
    dontwarn="$dontwarn -Wstack-protector"
    dontwarn="$dontwarn -Winline"
    dontwarn="$dontwarn -Wbad-function-cast"
    dontwarn="$dontwarn -Wshadow"
    dontwarn="$dontwarn -Wformat-signedness"
    dontwarn="$dontwarn -Wnull-dereference"

    # This causes an error to be detected in glib headers
    dontwarn="$dontwarn -Wshift-overflow=2"

    # We want to enable these, but need to sort out the
    # decl mess with  gtk/generated_*.c
    dontwarn="$dontwarn -Wmissing-declarations"

    # Stuff that C++ won't allow. Turn them back on later
    dontwarn="$dontwarn -Wdesignated-init"
    dontwarn="$dontwarn -Wdiscarded-array-qualifiers"
    dontwarn="$dontwarn -Wdiscarded-qualifiers"
    dontwarn="$dontwarn -Wimplicit"
    dontwarn="$dontwarn -Wimplicit-function-declaration"
    dontwarn="$dontwarn -Wimplicit-int"
    dontwarn="$dontwarn -Wincompatible-pointer-types"
    dontwarn="$dontwarn -Wint-conversion"
    dontwarn="$dontwarn -Wjump-misses-init"
    dontwarn="$dontwarn -Wmissing-parameter-type"
    dontwarn="$dontwarn -Wmissing-prototypes"
    dontwarn="$dontwarn -Wnested-externs"
    dontwarn="$dontwarn -Wold-style-declaration"
    dontwarn="$dontwarn -Wold-style-definition"
    dontwarn="$dontwarn -Woverride-init"
    dontwarn="$dontwarn -Wpointer-sign"
    dontwarn="$dontwarn -Wpointer-to-int-cast"
    dontwarn="$dontwarn -Wstrict-prototypes"
    dontwarn="$dontwarn -Wsuggest-final-methods"
    dontwarn="$dontwarn -Wsuggest-final-types"

    # Get all possible GCC warnings
    gl_MANYWARN_ALL_GCC([maybewarn])

    # Remove the ones we don't want, blacklisted earlier
    gl_MANYWARN_COMPLEMENT([wantwarn], [$maybewarn], [$dontwarn])

    # Check for $CC support of each warning
    for w in $wantwarn; do
      gl_WARN_ADD([$w])
    done

    # GNULIB uses '-W' (aka -Wextra) which includes a bunch of stuff.
    # Unfortunately, this means you can't simply use '-Wsign-compare'
    # with gl_MANYWARN_COMPLEMENT
    # So we have -W enabled, and then have to explicitly turn off...
    gl_WARN_ADD([-Wno-sign-compare])
    gl_WARN_ADD([-Wno-unused-parameter])
    # We can't enable this due to horrible spice_usb_device_get_description
    # signature
    gl_WARN_ADD([-Wno-format-nonliteral])

    # This should be < 1024 really. pixman_utils is the blackspot
    # preventing lower usage
    gl_WARN_ADD([-Wframe-larger-than=20460])

    # Use improved glibc headers
    AH_VERBATIM([FORTIFY_SOURCE],
    [/* Enable compile-time and run-time bounds-checking, and some warnings. */
#if !defined _FORTIFY_SOURCE &&  defined __OPTIMIZE__ && __OPTIMIZE__
# define _FORTIFY_SOURCE 2
#endif
])

    # Extra special flags
    dnl -fstack-protector stuff passes gl_WARN_ADD with gcc
    dnl on Mingw32, but fails when actually used
    case $host in
       *-*-linux*)
       dnl Fedora only uses -fstack-protector, but doesn't seem to
       dnl be great overhead in adding -fstack-protector-all instead
       dnl gl_WARN_ADD([-fstack-protector])
       gl_WARN_ADD([-fstack-protector-all])
       gl_WARN_ADD([--param=ssp-buffer-size=4])
       ;;
    esac
    gl_WARN_ADD([-fexceptions])
    gl_WARN_ADD([-fasynchronous-unwind-tables])
    gl_WARN_ADD([-fdiagnostics-show-option])
    gl_WARN_ADD([-funit-at-a-time])

    # Need -fipa-pure-const in order to make -Wsuggest-attribute=pure
    # fire even without -O.
    gl_WARN_ADD([-fipa-pure-const])

    # We should eventually enable this, but right now there are at
    # least 75 functions triggering warnings.
    gl_WARN_ADD([-Wno-suggest-attribute=pure])
    gl_WARN_ADD([-Wno-suggest-attribute=const])

    if test "$set_werror" = "yes"
    then
      gl_WARN_ADD([-Werror])
    fi

    save_CFLAGS="$WARN_CFLAGS"

    # -Wno-suggest-final-methods and -Wno-suggest-final-types to avoid warnings for optimization
    gl_WARN_ADD([-Wno-suggest-final-methods])
    gl_WARN_ADD([-Wno-suggest-final-types])

    # -Wno-array-bounds to avoid checks for array with 0 size
    gl_WARN_ADD([-Wno-array-bounds])

    # -Wno-narrowing to allow cast from -1 to unsigned (used in some initialization)
    gl_WARN_ADD([-Wno-narrowing])

    gl_WARN_ADD([-Wno-missing-field-initializers])

    # -Wshadow detects shadowing of arguments, globals and C++ attributes
    gl_WARN_ADD([-Wshadow])

    WARN_CXXFLAGS="$WARN_CFLAGS"
    AC_SUBST([WARN_CXXFLAGS])

    WARN_CFLAGS="$save_CFLAGS"
    WARN_LDFLAGS=$WARN_CFLAGS
    AC_SUBST([WARN_CFLAGS])
    AC_SUBST([WARN_LDFLAGS])
])
