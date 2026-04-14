#!/bin/bash
# Kotuku Debian Package Builder
# This script builds .deb packages for Kotuku

set -e -u -o pipefail

# Enable debug mode if DEBUG environment variable is set
[[ "${DEBUG:-}" == "1" ]] && set -x

# Default values
BUILD_DIR="build-deb"
INSTALL_INCLUDES="OFF"
CLEAN_BUILD="no"
SIGN_PACKAGE="no"

# Function to escape shell metacharacters in changelog entries
escape_changelog_line() {
    local line="$1"
    # Remove or escape potentially dangerous characters for shell safety
# Function to robustly quote changelog entries for shell safety
escape_changelog_line() {
    local line="$1"
    # Use single quotes and escape embedded single quotes
    # This produces a shell-safe, literal string
    printf "'%s'" "$(echo "$line" | sed "s/'/'\\\\''/g")"
}

# Function to generate changelog entries from Git log
generate_changelog_entries() {
    local max_entries="${1:-10}"

    if command -v git &> /dev/null && git rev-parse --git-dir &> /dev/null; then
        # Use quote-safe Git log formatting and escape output for security
        local git_output
        git_output=$(git log --oneline --pretty=format:"%s" -n "$max_entries" 2>/dev/null) || {
            echo "  * Initial release"
            return
        }

        # Process each line safely to prevent command injection
        if [ -n "$git_output" ]; then
            while IFS= read -r line; do
                if [ -n "$line" ]; then
                    echo "  * $(escape_changelog_line "$line")"
                fi
            done <<< "$git_output"
        else
            echo "  * Initial release"
        fi
    else
        echo "  * Package built from source"
    fi
}

# Function to process control file templates
process_control_template() {
    local template_file="$1"
    local output_file="$2"
    local package_version="$3"
    local architecture="$4"

    if [[ ! -f "$template_file" ]]; then
        echo "Error: Control template not found: $template_file"
        return 1
    fi

    # Get current date in RFC 2822 format for Debian changelog
    local current_date
    current_date=$(date -R 2>/dev/null || date)

    # Process template variables with proper escaping
    sed -e "s/@VERSION@/$package_version/g" \
        -e "s/@DEBIAN_ARCH@/$architecture/g" \
        -e "s/@INSTALL_INCLUDES@/ON/g" \
        -e "s/@NPROC@/${JOBS:-4}/g" \
        -e "s/@DATE@/$current_date/g" \
        "$template_file" > "$output_file" || {
        echo "Error: Failed to process control template"
        return 1
    }
}

# Function to create Debian changelog from template
create_debian_changelog() {
    local output_file="$1"
    local package_name="$2"
    local version="$3"

    cat > "$output_file" << EOF
$package_name ($version-1) stable; urgency=medium

$(generate_changelog_entries 5)

 -- Kotuku Open Group <team@kotuku.dev>  $(date -R)
EOF
}

# Function to show usage
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -h, --help              Show this help message"
    echo "  -c, --clean             Clean build directory before building"
    echo "  -d, --dev               Build development package (includes headers)"
    echo "  -s, --sign              Sign the package (requires GPG setup)"
    echo "  -b, --build-dir DIR     Use custom build directory (default: build-deb)"
    echo ""
    echo "Examples:"
    echo "  $0                      Build runtime package only"
    echo "  $0 --dev               Build both runtime and development packages"
    echo "  $0 --clean --dev       Clean build and create both packages"
}

# Parse command line arguments with input validation
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -c|--clean)
            CLEAN_BUILD="yes"
            shift
            ;;
        -d|--dev)
            INSTALL_INCLUDES="ON"
            shift
            ;;
        -s|--sign)
            SIGN_PACKAGE="yes"
            shift
            ;;
        -b|--build-dir)
            if [[ -z "${2:-}" ]]; then
                echo "Error: --build-dir requires a directory argument"
                exit 1
            fi
            # Sanitize and validate build directory path
            BUILD_DIR="$(realpath "$2" 2>/dev/null || readlink -f "$2" 2>/dev/null || echo "$2")"
            if [[ "$BUILD_DIR" =~ [\;\&\|\`\$] ]]; then
                echo "Error: Build directory path contains invalid characters"
                exit 1
            fi

            # Ensure the parent directory exists for the build directory
            local parent_dir
            parent_dir="$(dirname "$BUILD_DIR")"
            if [[ ! -d "$parent_dir" ]]; then
                echo "Error: Parent directory for build path does not exist: $parent_dir"
                exit 1
            fi
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Function to validate build dependencies
validate_build_dependencies() {
    local missing_deps=()
    local missing_packages=()

    echo "Validating build dependencies..."

    # Essential build tools
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
        missing_packages+=("cmake")
    fi

    if ! command -v dpkg-deb &> /dev/null; then
        missing_deps+=("dpkg-deb")
        missing_packages+=("dpkg-dev")
    fi

    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
        missing_packages+=("build-essential")
    fi

    if ! command -v gcc &> /dev/null; then
        missing_deps+=("gcc")
        missing_packages+=("build-essential")
    fi

    # Check for debhelper (used by proper Debian packaging)
    if ! command -v dh &> /dev/null; then
        missing_deps+=("debhelper")
        missing_packages+=("debhelper")
    fi

    # Development libraries (check for pkg-config files or headers)
    if ! pkg-config --exists freetype2 2>/dev/null && [[ ! -f /usr/include/freetype2/freetype/freetype.h ]]; then
        missing_deps+=("libfreetype6-dev")
        missing_packages+=("libfreetype6-dev")
    fi

    if [[ ! -f /usr/include/zlib.h ]]; then
        missing_deps+=("zlib1g-dev")
        missing_packages+=("zlib1g-dev")
    fi

    if [[ ! -f /usr/include/X11/Xlib.h ]]; then
        missing_deps+=("libx11-dev")
        missing_packages+=("libx11-dev")
    fi

    if [[ ! -f /usr/include/alsa/asoundlib.h ]]; then
        missing_deps+=("libasound2-dev")
        missing_packages+=("libasound2-dev")
    fi

    # Report missing dependencies
    if [ ${#missing_deps[@]} -gt 0 ]; then
        echo "Error: Missing required build dependencies:"
        printf "  - %s\\n" "${missing_deps[@]}"
        echo ""
        echo "Install missing packages with:"
        echo "  sudo apt update"
        echo "  sudo apt install $(printf "%s " "${missing_packages[@]}" | sort -u | xargs)"
        echo ""
        return 1
    fi

    echo "✓ All build dependencies satisfied"
    return 0
}

# Validate dependencies before proceeding
if ! validate_build_dependencies; then
    exit 1
fi

# Get version from CMakeLists.txt with validation
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: CMakeLists.txt not found in current directory"
    echo "Please run this script from the Kotuku source root directory"
    exit 1
fi

VERSION=$(awk '
    BEGIN { version = "" }
    # Match project(Kotuku ... VERSION x.y.z ...)
    /project[[:space:]]*\([[:space:]]*Kotuku[[:space:]]+VERSION[[:space:]]*[0-9]+\.[0-9]+\.[0-9]+/ {
        match($0, /VERSION[[:space:]]*([0-9]+\.[0-9]+\.[0-9]+)/, arr)
        if (arr[1] != "") { version = arr[1] }
    }
    END { print version }
' CMakeLists.txt)
if [ -z "$VERSION" ] || [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Could not extract valid version from CMakeLists.txt"
    echo "Expected format: project (Kotuku VERSION x.y.z)"
    exit 1
fi

# Detect architecture
ARCH=$(dpkg --print-architecture)

echo "Building Kotuku Debian packages..."
echo "Version: $VERSION"
echo "Architecture: $ARCH"
echo "Build directory: $BUILD_DIR"
echo "Include headers: $INSTALL_INCLUDES"

# Clean build directory if requested
if [ "$CLEAN_BUILD" = "yes" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Configure CMake with Debian package generation
echo "Configuring CMake..."
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DGENERATE_DEBIAN_PKG=ON \
    -DKOTUKU_STATIC=OFF \
    -DRUN_ANYWHERE=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_DEFS=OFF \
    -DINSTALL_EXAMPLES=OFF \
    -DINSTALL_INCLUDES="$INSTALL_INCLUDES" \
    -DINSTALL_TESTS=OFF

# Build the project with error checking
echo "Building project..."
# Validate and use nproc with fallback
JOBS=$(nproc 2>/dev/null || echo "4")
if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [ "$JOBS" -eq 0 ]; then
    echo "Warning: Invalid nproc output, using default of 4 jobs"
    JOBS=4
fi

if ! cmake --build "$BUILD_DIR" --config Release -j "$JOBS"; then
    echo "Error: Build failed. Check build output above for details."
    exit 1
fi

# Create staging directories for packages
RUNTIME_STAGING="$BUILD_DIR/debian/kotuku"
DEV_STAGING="$BUILD_DIR/debian/kotuku-dev"

echo "Creating package staging directories..."
rm -rf "$RUNTIME_STAGING" "$DEV_STAGING"
mkdir -p "$RUNTIME_STAGING/DEBIAN" "$RUNTIME_STAGING/usr"

if [ "$INSTALL_INCLUDES" = "ON" ]; then
    mkdir -p "$DEV_STAGING/DEBIAN" "$DEV_STAGING/usr"
fi

# Install to staging directory with error checking
echo "Installing to staging directory..."
if ! DESTDIR="$RUNTIME_STAGING" cmake --install "$BUILD_DIR"; then
    echo "Error: Installation failed"
    exit 1
fi

# Copy debian control files for runtime package
# Copy control file if it exists (generated by CMake)
if [ -f "$BUILD_DIR/debian/control" ]; then
    cp "$BUILD_DIR/debian/control" "$RUNTIME_STAGING/DEBIAN/" || { echo "Failed to copy control file"; exit 1; }
fi
if [ ! -d "$RUNTIME_STAGING/usr/share/doc/kotuku/" ]; then
    mkdir -p "$RUNTIME_STAGING/usr/share/doc/kotuku/" || { echo "Failed to create directory $RUNTIME_STAGING/usr/share/doc/kotuku/"; exit 1; }
fi
# Copy or generate documentation files
if [ -f "$BUILD_DIR/debian/copyright" ]; then
    cp "$BUILD_DIR/debian/copyright" "$RUNTIME_STAGING/usr/share/doc/kotuku/" || { echo "Failed to copy copyright file"; exit 1; }
fi

# Generate changelog if not provided by CMake
if [ -f "$BUILD_DIR/debian/changelog" ]; then
    cp "$BUILD_DIR/debian/changelog" "$RUNTIME_STAGING/usr/share/doc/kotuku/" || { echo "Failed to copy changelog file"; exit 1; }
else
    create_debian_changelog "$RUNTIME_STAGING/usr/share/doc/kotuku/changelog" "kotuku" "$VERSION"
fi

# Copy maintainer scripts if they exist
for script in postinst prerm postrm; do
    if [ -f "pkg/debian/${script}.in" ]; then
        cp "pkg/debian/${script}.in" "$RUNTIME_STAGING/DEBIAN/${script}" || { echo "Warning: Failed to copy ${script} script"; }
        chmod 755 "$RUNTIME_STAGING/DEBIAN/${script}" 2>/dev/null || true
    fi
done

# Copy lintian overrides if they exist
if [ -f "pkg/debian/kotuku.lintian-overrides" ]; then
    mkdir -p "$RUNTIME_STAGING/usr/share/lintian/overrides"
    cp "pkg/debian/kotuku.lintian-overrides" "$RUNTIME_STAGING/usr/share/lintian/overrides/kotuku" || { echo "Warning: Failed to copy lintian overrides"; }
fi

# Create runtime package control file from template
if ! process_control_template "pkg/debian/control-runtime.in" "$RUNTIME_STAGING/DEBIAN/control" "$VERSION" "$ARCH"; then
    echo "Error: Failed to create runtime control file"
    exit 1
fi

# Handle development package if requested
if [ "$INSTALL_INCLUDES" = "ON" ]; then
    echo "Setting up development package..."

    # Move headers and development files to dev package
    if [ -d "$RUNTIME_STAGING/usr/include" ]; then
        mkdir -p "$DEV_STAGING/usr"
        mv "$RUNTIME_STAGING/usr/include" "$DEV_STAGING/usr/"
    fi

    # Create dev package control file from template
    if ! process_control_template "pkg/debian/control-dev.in" "$DEV_STAGING/DEBIAN/control" "$VERSION" "$ARCH"; then
        echo "Error: Failed to create dev control file"
        exit 1
    fi

    # Copy documentation to dev package
    mkdir -p "$DEV_STAGING/usr/share/doc/kotuku-dev"
    if [ -f "$BUILD_DIR/debian/copyright" ]; then
        cp "$BUILD_DIR/debian/copyright" "$DEV_STAGING/usr/share/doc/kotuku-dev/"
    fi

    # Generate or copy changelog
    if [ -f "$BUILD_DIR/debian/changelog" ]; then
        cp "$BUILD_DIR/debian/changelog" "$DEV_STAGING/usr/share/doc/kotuku-dev/"
    else
        create_debian_changelog "$DEV_STAGING/usr/share/doc/kotuku-dev/changelog" "kotuku-dev" "$VERSION"
    fi

    # Copy lintian overrides for dev package if they exist
    if [ -f "pkg/debian/kotuku-dev.lintian-overrides" ]; then
        mkdir -p "$DEV_STAGING/usr/share/lintian/overrides"
        cp "pkg/debian/kotuku-dev.lintian-overrides" "$DEV_STAGING/usr/share/lintian/overrides/kotuku-dev" || { echo "Warning: Failed to copy dev lintian overrides"; }
    fi
fi

# Build the packages
echo "Building .deb packages..."

# Build runtime package
RUNTIME_PACKAGE="kotuku_${VERSION}-1_${ARCH}.deb"
dpkg-deb --build "$RUNTIME_STAGING" "$RUNTIME_PACKAGE"
echo "Created: $RUNTIME_PACKAGE"

# Build development package if requested
if [ "$INSTALL_INCLUDES" = "ON" ]; then
    DEV_PACKAGE="kotuku-dev_${VERSION}-1_${ARCH}.deb"
    dpkg-deb --build "$DEV_STAGING" "$DEV_PACKAGE"
    echo "Created: $DEV_PACKAGE"
fi

# Sign packages if requested
if [ "$SIGN_PACKAGE" = "yes" ]; then
    if command -v dpkg-sig &> /dev/null; then
        echo "Signing packages..."
        dpkg-sig --sign builder "$RUNTIME_PACKAGE"
        if [ "$INSTALL_INCLUDES" = "ON" ]; then
            dpkg-sig --sign builder "$DEV_PACKAGE"
        fi
        echo "Packages signed successfully"
    else
        echo "Warning: dpkg-sig not found, packages not signed"
        echo "Install with: sudo apt install dpkg-sig"
    fi
fi

# Verify packages
echo "Verifying packages..."
dpkg-deb -I "$RUNTIME_PACKAGE"
if [ "$INSTALL_INCLUDES" = "ON" ]; then
    dpkg-deb -I "$DEV_PACKAGE"
fi

echo ""
echo "Debian packages built successfully!"
echo "Runtime package: $RUNTIME_PACKAGE"
if [ "$INSTALL_INCLUDES" = "ON" ]; then
    echo "Development package: $DEV_PACKAGE"
fi
echo ""
echo "To install:"
echo "  sudo dpkg -i $RUNTIME_PACKAGE"
if [ "$INSTALL_INCLUDES" = "ON" ]; then
    echo "  sudo dpkg -i $DEV_PACKAGE"
fi
echo ""
echo "To install dependencies if needed:"
echo "  sudo apt-get install -f"