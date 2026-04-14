#
# Kotuku Windows Release Build Script
#
# This script builds a Windows release package using CMake and CPack
# Produces the same portable ZIP distribution as the existing build process
#
# For SFX support run: choco install 7zip
#
# Example: pkg\build-release.ps1 -BuildType Release -Static -Package -CreateSFX
#
# To override defaults, use -ParameterName:$false to disable a switch:
#
# Skip packaging, just build:  pkg\build-release.ps1 -Package:$false
# Build without SFX, include headers: pkg\build-release.ps1 -CreateSFX:$false -IncludeHeaders
# Modular build with SSL disabled: pkg\build-release.ps1 -Static:$false -DisableSSL
# Limit parallel jobs: pkg\build-release.ps1 -ParallelJobs 4

param(
    [string]$BuildType = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [switch]$Static = $true,
    [switch]$DisableSSL = $false,
    [switch]$IncludeExamples = $true,
    [switch]$IncludeHeaders = $false,
    [switch]$Clean = $true,
    [switch]$Package = $true,
    [switch]$CreateSFX = $true,
    [int]$ParallelJobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# Resolve the CMake cpack executable (Chocolatey's cpack.exe can shadow it)
$cmakePath = (Get-Command cmake -ErrorAction Stop).Source | Split-Path
$cpackExe = Join-Path $cmakePath "cpack.exe"
if (-not (Test-Path $cpackExe)) {
    Write-Error "Could not find CMake's cpack.exe at $cpackExe"
}
Write-Host "Using CPack: $cpackExe" -ForegroundColor DarkGray

# Generate timestamp for consistent naming
$timestamp = Get-Date -Format "yyyyMMdd"
$buildDir = "release-$timestamp"
$packageName = "kotuku-win64-$timestamp"

Write-Host "=== Kotuku Framework Windows Release Build ===" -ForegroundColor Cyan
Write-Host "Build Type: $BuildType" -ForegroundColor Yellow
Write-Host "Static Build: $Static" -ForegroundColor Yellow
Write-Host "Generator: $Generator" -ForegroundColor Yellow
Write-Host "Package Name: $packageName" -ForegroundColor Yellow
Write-Host "Parallel Jobs: $ParallelJobs" -ForegroundColor Yellow
Write-Host "Create SFX: $CreateSFX" -ForegroundColor Yellow
Write-Host ""

# Clean previous builds if requested
if ($Clean) {
    Write-Host "Cleaning previous builds..." -ForegroundColor Green
    Remove-Item -Path $buildDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path "$packageName*" -Force -ErrorAction SilentlyContinue
    Write-Host "Clean completed." -ForegroundColor Green
    Write-Host ""
}

# Configure CMake build
Write-Host "Configuring CMake build..." -ForegroundColor Green

$cmakeArgs = @(
    "-S", ".",
    "-B", $buildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DKOTUKU_STATIC=$($Static.ToString().ToUpper())",
    "-DBUILD_DEFS=OFF",
    "-DINSTALL_EXAMPLES=$($IncludeExamples.ToString().ToUpper())",
    "-DINSTALL_INCLUDES=$($IncludeHeaders.ToString().ToUpper())",
    "-DDISABLE_SSL=$($DisableSSL.ToString().ToUpper())",
    "-DCMAKE_INSTALL_PREFIX=install",
    "-G", $Generator
)

Write-Host "CMake command: cmake $($cmakeArgs -join ' ')" -ForegroundColor DarkGray
& cmake @cmakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed with exit code $LASTEXITCODE"
}

Write-Host "CMake configuration completed successfully." -ForegroundColor Green
Write-Host ""

# Build the project
Write-Host "Building project with $ParallelJobs parallel jobs..." -ForegroundColor Green

& cmake --build $buildDir --config $BuildType -j $ParallelJobs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Build completed successfully." -ForegroundColor Green
Write-Host ""

# Install to prepare for packaging
Write-Host "Installing to staging directory..." -ForegroundColor Green

& cmake --install $buildDir --config $BuildType

if ($LASTEXITCODE -ne 0) {
    Write-Error "Install failed with exit code $LASTEXITCODE"
}

Write-Host "Installation completed successfully." -ForegroundColor Green
Write-Host ""

# Create packages using CPack
if ($Package) {
    Write-Host "Creating packages with CPack..." -ForegroundColor Green

    Push-Location $buildDir

    try {
        # Create ZIP package (portable distribution)
        Write-Host "  - Creating ZIP archive..." -ForegroundColor Yellow
        & $cpackExe -G ZIP -C $BuildType

        if ($LASTEXITCODE -ne 0) {
            Write-Warning "ZIP package creation failed with exit code $LASTEXITCODE"
        } else {
            Write-Host "  - ZIP package created successfully." -ForegroundColor Green
        }

        # Create NSIS installer if not static build
        if (-not $Static) {
            Write-Host "  - Creating NSIS installer..." -ForegroundColor Yellow
            & $cpackExe -G NSIS -C $BuildType

            if ($LASTEXITCODE -ne 0) {
                Write-Warning "NSIS installer creation failed with exit code $LASTEXITCODE"
            } else {
                Write-Host "  - NSIS installer created successfully." -ForegroundColor Green
            }
        }

        # Create Self-Extracting Archive if requested (requires 7-Zip to be found during CMake configure)
        if ($CreateSFX) {
            $sfxConfigLog = Get-Content "CMakeCache.txt" -ErrorAction SilentlyContinue | Select-String "SEVENZIP_EXECUTABLE"
            if ($sfxConfigLog -and $sfxConfigLog -notmatch "NOTFOUND") {
                Write-Host "  - Creating Self-Extracting Archive..." -ForegroundColor Yellow
                & cmake --build . --target package_sfx --config $BuildType

                if ($LASTEXITCODE -ne 0) {
                    Write-Warning "Self-Extracting Archive creation failed with exit code $LASTEXITCODE"
                } else {
                    Write-Host "  - Self-Extracting Archive created successfully." -ForegroundColor Green
                }
            } else {
                Write-Warning "SFX skipped - 7-Zip was not found during CMake configuration"
            }
        }

        # List generated packages
        Write-Host ""
        Write-Host "Generated packages:" -ForegroundColor Cyan
        Get-ChildItem -Path "." -Filter "*kotuku*" -File | ForEach-Object {
            $sizeKB = [Math]::Round($_.Length / 1KB, 1)
            Write-Host "  - $($_.Name) ($sizeKB KB)" -ForegroundColor Yellow
        }

    } finally {
        Pop-Location
    }

    # Copy packages to root directory for easy access
    Write-Host ""
    Write-Host "Copying packages to root directory..." -ForegroundColor Green
    Get-ChildItem -Path "$buildDir" -Filter "*kotuku*" -File | Copy-Item -Destination "." -Force

} else {
    Write-Host "Packaging skipped (use -Package to enable)." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== Build Summary ===" -ForegroundColor Cyan
Write-Host "Build Directory: $buildDir" -ForegroundColor Yellow
Write-Host "Install Directory: $buildDir/install" -ForegroundColor Yellow

if ($Package) {
    $finalPackages = Get-ChildItem -Path "." -Filter "*kotuku*" -File | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-5) }
    if ($finalPackages) {
        Write-Host "Final Packages:" -ForegroundColor Yellow
        $finalPackages | ForEach-Object {
            $sizeKB = [Math]::Round($_.Length / 1KB, 1)
            Write-Host "  - $($_.Name) ($sizeKB KB)" -ForegroundColor Green
        }
    }
}

Write-Host ""
Write-Host "Windows release build completed successfully!" -ForegroundColor Green
