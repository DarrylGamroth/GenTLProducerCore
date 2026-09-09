set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "libgentl-producer-core-dev")
set(CPACK_PACKAGE_VENDOR "Darryl Gamroth")
set(CPACK_PACKAGE_CONTACT "Darryl Gamroth <dgamroth@rubus.ca>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Static support library for implementing GenTL producers")
set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/DarrylGamroth/GenTLProducerCore")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "cmake (>= 3.20), ca-certificates")

include(CPack)
