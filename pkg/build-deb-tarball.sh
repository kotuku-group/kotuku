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
    echo "$line" | sed 's/[`$\\]/\\&/g; s/[|&;(){}]/\\&/g'
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
        echo "  * Package built from pre-compiled release"
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

    # Process template variables with proper escaping
    sed -e "s/@VERSION@/$package_version/g" \
        -e "s/@DEBIAN_ARCH@/$architecture/g" \
        -e "s/@INSTALL_INCLUDES@/ON/g" \
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
    echo "Usage: $0 <tarball> [options]"
    echo "Arguments:"
    echo "  tarball                 Path to .tar.gz file containing compiled Kotuku release"
    echo "Options:"
    echo "  -h, --help              Show this help message"
    echo "  -c, --clean             Clean build directory before building"
    echo "  -d, --dev               Build development package (includes headers)"
    echo "  -s, --sign              Sign the package (requires GPG setup)"
    echo "  -b, --build-dir DIR     Use custom build directory (default: build-deb)"
    echo ""
    echo "Examples:"
    echo "  $0 kotuku-linux64-20250731.tar.gz          Build runtime package"
    echo "  $0 kotuku-linux64-20250731.tar.gz --dev    Build both runtime and dev packages"
    echo "  $0 kotuku-linux64-20250731.tar.gz --clean  Clean build and create package"
}

# Check if tarball is provided
if [[ $# -eq 0 ]]; then
    echo "Error: No tarball specified"
    usage
    exit 1
fi

# Sanitize and validate tarball path
TARBALL="$(realpath "$1" 2>/dev/null)"
if [[ -z "$TARBALL" ]]; then
    echo "Error: Failed to resolve absolute path for tarball: $1"
    exit 1
fi
shift

# Validate tarball path doesn't contain dangerous characters
if [[ "$TARBALL" =~ [\;\&\|\`] ]]; then
    echo "Error: Tarball path contains invalid characters"
    exit 1
fi

# Validate tarball exists and is readable
if [[ ! -f "$TARBALL" ]]; then
    echo "Error: Tarball '$TARBALL' not found"
    exit 1
fi

if [[ ! -r "$TARBALL" ]]; then
    echo "Error: Tarball '$TARBALL' is not readable"
    exit 1
fi

# Validate tarball format
if [[ ! "$TARBALL" =~ \.tar\.gz$ ]]; then
    echo "Error: Tarball must be a .tar.gz file"
    exit 1
fi

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

# Function to validate build dependencies for tarball packaging
validate_tarball_dependencies() {
    local missing_deps=()
    local missing_packages=()

    echo "Validating packaging dependencies..."

    # Essential packaging tools
    if ! command -v dpkg-deb &> /dev/null; then
        missing_deps+=("dpkg-deb")
        missing_packages+=("dpkg-dev")
    fi

    if ! command -v tar &> /dev/null; then
        missing_deps+=("tar")
        missing_packages+=("tar")
    fi

    # Report missing dependencies (only fail on essential tools, not debhelper)
    if [ ${#missing_deps[@]} -gt 0 ]; then
        echo "Error: Missing required packaging dependencies:"
        printf "  - %s\\n" "${missing_deps[@]}"
        echo ""
        echo "Install missing packages with:"
        echo "  sudo apt update"
        echo "  sudo apt install $(printf "%s " "${missing_packages[@]}" | sort -u | xargs)"
        echo ""
        return 1
    fi

    # Warn if debhelper is missing (optional for tarball builds)
    if ! command -v dh &> /dev/null; then
        echo "⚠ debhelper not found (optional for tarball builds)"
    fi

    echo "✓ All essential packaging dependencies satisfied"
    return 0
}

# Validate dependencies before proceeding
if ! validate_tarball_dependencies; then
    exit 1
fi

# Extract version from tarball filename
TARBALL_BASENAME=$(basename "$TARBALL" .tar.gz)
DATE_VERSION=$(echo "$TARBALL_BASENAME" | sed -n 's/.*-\([0-9]\{8\}\).*/\1/p')

if [ -n "$DATE_VERSION" ]; then
    # Use date-based version format
    VERSION="$DATE_VERSION"
else
    # Fallback: try to get version from CMakeLists.txt if available
    if [ -f "CMakeLists.txt" ]; then
        VERSION=$(grep "project (Kotuku VERSION" CMakeLists.txt | sed 's/.*VERSION \([0-9.]*\).*/\1/')
    fi
    if [ -z "$VERSION" ]; then
        echo "Error: Could not extract version from tarball filename or CMakeLists.txt"
        echo "Expected tarball format: kotuku-linux64-YYYYMMDD.tar.gz"
        exit 1
    fi
fi

# Detect architecture
ARCH=$(dpkg --print-architecture)

echo "Building Kotuku Debian packages from tarball..."
echo "Tarball: $TARBALL"
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

# Extract tarball to build directory with validation
echo "Extracting tarball..."
mkdir -p "$BUILD_DIR/extracted" || { echo "Error: Failed to create extraction directory"; exit 1; }

if ! tar -xzf "$TARBALL" -C "$BUILD_DIR/extracted" --strip-components=1; then
    echo "Error: Failed to extract tarball. The file may be corrupted."
    exit 1
fi

# Verify essential files exist in extracted tarball
EXECUTABLE_NAME=""
if [[ -f "$BUILD_DIR/extracted/kotuku" ]]; then
    EXECUTABLE_NAME="kotuku"
elif [[ -f "$BUILD_DIR/extracted/origo" ]]; then
    EXECUTABLE_NAME="origo"
elif [[ -f "$BUILD_DIR/extracted/tiri" ]]; then
    EXECUTABLE_NAME="tiri"
else
    echo "Error: No executable found in tarball (looked for kotuku, origo, or tiri)"
    exit 1
fi

echo "Tarball extracted to $BUILD_DIR/extracted (executable: $EXECUTABLE_NAME)"

# Create staging directories for packages
RUNTIME_STAGING="$BUILD_DIR/debian/kotuku"
DEV_STAGING="$BUILD_DIR/debian/kotuku-dev"

echo "Creating package staging directories..."
rm -rf "$RUNTIME_STAGING" "$DEV_STAGING"
mkdir -p "$RUNTIME_STAGING/DEBIAN" "$RUNTIME_STAGING/usr"

if [ "$INSTALL_INCLUDES" = "ON" ]; then
    mkdir -p "$DEV_STAGING/DEBIAN" "$DEV_STAGING/usr"
fi

# Copy files from extracted tarball to staging directory
echo "Installing to staging directory..."

# Create necessary directories
mkdir -p "$RUNTIME_STAGING/usr/bin"
mkdir -p "$RUNTIME_STAGING/usr/share/kotuku"
mkdir -p "$RUNTIME_STAGING/usr/share/doc/kotuku"

# Copy the main executable binary with validation
if [[ ! -f "$BUILD_DIR/extracted/$EXECUTABLE_NAME" ]]; then
    echo "Error: Executable not found in extracted files: $EXECUTABLE_NAME"
    exit 1
fi

# Copy the executable as 'origo' for consistency
cp "$BUILD_DIR/extracted/$EXECUTABLE_NAME" "$RUNTIME_STAGING/usr/bin/origo" || { echo "Error: Failed to copy executable"; exit 1; }
chmod 755 "$RUNTIME_STAGING/usr/bin/origo" || { echo "Error: Failed to set executable permissions"; exit 1; }

# Create a symlink with the original name if different
if [[ "$EXECUTABLE_NAME" != "origo" ]]; then
    ln -s origo "$RUNTIME_STAGING/usr/bin/$EXECUTABLE_NAME" || { echo "Warning: Failed to create symlink for $EXECUTABLE_NAME"; }
fi

# Copy configuration and scripts with validation
if [[ -d "$BUILD_DIR/extracted/config" ]]; then
    cp -r "$BUILD_DIR/extracted/config" "$RUNTIME_STAGING/usr/share/kotuku/" || { echo "Warning: Failed to copy config directory"; }
else
    echo "Warning: No config directory found in tarball"
fi

if [[ -d "$BUILD_DIR/extracted/scripts" ]]; then
    cp -r "$BUILD_DIR/extracted/scripts" "$RUNTIME_STAGING/usr/share/kotuku/" || { echo "Warning: Failed to copy scripts directory"; }
else
    echo "Warning: No scripts directory found in tarball"
fi

# Copy examples if they exist
if [ -d "$BUILD_DIR/extracted/examples" ]; then
    cp -r "$BUILD_DIR/extracted/examples" "$RUNTIME_STAGING/usr/share/kotuku/"
fi

# Create copyright and changelog files
cat > "$RUNTIME_STAGING/usr/share/doc/kotuku/copyright" << 'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Kotuku
Upstream-Contact: Kotuku Open Group <team@kotuku.dev>
Source: https://github.com/parasol-framework/kotuku

Files: *
Copyright: Paul Manias
License: Custom
 This package is distributed under the terms described in the LICENSE.TXT file
 that is distributed with this package. Please refer to it for further
 information on licensing.
EOF

# Generate changelog with Git history if available
create_debian_changelog "$RUNTIME_STAGING/usr/share/doc/kotuku/changelog" "kotuku" "$VERSION"

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

    # Create copyright file for dev package
    cat > "$DEV_STAGING/usr/share/doc/kotuku-dev/copyright" << 'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Kotuku
Upstream-Contact: Kotuku Open Group <team@kotuku.dev>
Source: https://github.com/parasol-framework/kotuku

Files: *
Copyright: Paul Manias
License: Custom
 This package is distributed under the terms described in the LICENSE.TXT file
 that is distributed with this package. Please refer to it for further
 information on licensing.
EOF

    # Generate changelog for dev package
    create_debian_changelog "$DEV_STAGING/usr/share/doc/kotuku-dev/changelog" "kotuku-dev" "$VERSION"
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