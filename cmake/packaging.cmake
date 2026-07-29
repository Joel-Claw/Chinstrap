# =========================================================================
# CPack Packaging Configuration
# =========================================================================
# TEACHING NOTE: CPack is CMake's built-in packaging tool. It takes the
# build artifacts (the compiled binary, headers, assets) and bundles them
# into distributable packages. We support two formats here:
#
#   1. .tar.gz - A plain tarball. Universal across all Linux distros.
#                Good for "portable" installs and source distributions.
#
#   2. .deb    - Debian package format. Used by Debian, Ubuntu, Mint, and
#                other apt-based distributions. The .deb contains metadata
#                (control file), post-install scripts, and the file payload.
#
# CPack works by reading variables you set here, then running a generator
# (TGZ for tarballs, DEB for Debian packages) at "make package" time.
#
# The flow is:
#   cmake ..          -> configure build, CPack reads these variables
#   make              -> compile the binary
#   make package      -> CPack gathers installed files, creates .tar.gz and .deb
#
# On Arch Linux, .deb is less useful, but including both generators lets
# us produce packages for the two most common packaging ecosystems from
# a single build directory.
# =========================================================================

# -------------------------------------------------------------------------
# Package metadata - These fields appear in the package manager UI and
# in package inspection tools like "dpkg -I chinstrap.deb".
# -------------------------------------------------------------------------

# Human-readable name shown in package managers
set(CPACK_PACKAGE_NAME "chinstrap")

# Version string. CMake fills PROJECT_VERSION from the project() command
# above, so this stays in sync automatically.
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

# One-line summary. Shows up in "dpkg -I" and in apt search results.
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A from-scratch web browser with zero third-party libraries")

# Multi-line description. Some packagers display this in "Details" views.
set(CPACK_PACKAGE_DESCRIPTION
"Chinstrap is a web browser written entirely in C++17 with no external\n"
"dependencies. It implements its own HTML parser, CSS engine, JavaScript\n"
"interpreter, TLS stack, HTTP client, and rendering pipeline from scratch.\n"
".\n"
"Features:\n"
" - HTML5 parsing\n"
" - CSS3 parsing and layout\n"
" - JavaScript ES6 interpreter\n"
" - TLS 1.2/1.3 encryption\n"
" - HTTP/1.1 and HTTP/2 support\n"
" - DNS resolution\n"
" - Cookie management\n"
" - Disk cache\n"
" - X11 display backend\n"
)

# Contact info for the packager. Replace with the actual maintainer.
set(CPACK_PACKAGE_CONTACT "Chinstrap Project <chinstrap@example.com>")

# Homepage URL. Shown in package managers that support it.
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/chinstrap/chinstrap")

# License identifier. This should match the LICENSE file in the repo.
set(CPACK_PACKAGE_LICENSE "MIT")

# -------------------------------------------------------------------------
# Package file naming - Controls the output filename of generated packages.
# CPack_PACKAGE_FILE_NAME is the template: name-version-architecture
# -------------------------------------------------------------------------

# Detect the target architecture so the package filename includes it.
# CMAKE_SYSTEM_PROCESSOR is set by CMake (e.g., x86_64, aarch64).
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}"
)

# -------------------------------------------------------------------------
# Generator selection - Which package formats to produce.
# -------------------------------------------------------------------------

# List of generators to run. Each produces one package file.
#   TGZ = .tar.gz tarball (universal)
#   DEB = .deb package    (Debian/Ubuntu)
set(CPACK_GENERATOR "TGZ;DEB")

# -------------------------------------------------------------------------
# Debian-specific settings (only used when the DEB generator runs)
# -------------------------------------------------------------------------

# The section in Debian package classification. "web" is appropriate for
# a browser. Other common sections: utils, net, devel, x11.
set(CPACK_DEBIAN_PACKAGE_SECTION "web")

# Dependencies required at runtime. The browser needs:
#   libx11-6  - X11 client library (we link against X11 at runtime)
#   libm      - math library (usually part of libc, but listing explicitly)
# Note: we do NOT depend on any third-party browser libraries. That is
# the whole point of Chinstrap.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6")

# Architecture field. "any" means this package works on any arch, but since
# we ship a compiled binary, we use the actual build architecture.
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")

# Priority: "optional" is the standard for user-facing applications.
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

# -------------------------------------------------------------------------
# Tarball-specific settings (only used when the TGZ generator runs)
# -------------------------------------------------------------------------

# The TGZ generator has very few options - it just tars up the install
# prefix. No extra configuration needed beyond the common variables above.

# -------------------------------------------------------------------------
# Components (optional) - We use a single "main" component for simplicity.
# If the project grows, you can split into Runtime/Development/Docs.
# -------------------------------------------------------------------------
set(CPACK_COMPONENTS_GROUPING "ONE_PER_GROUP")
set(CPACK_COMPONENTS_ALL "main")