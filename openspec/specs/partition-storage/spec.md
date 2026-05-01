## ADDED Requirements

### Requirement: Dedicated storage partition for SPIFFS
The firmware SHALL include a dedicated flash partition for SPIFFS filesystem storage, separate from the application partition.

#### Scenario: Partition table includes storage partition
- **WHEN** the firmware is built
- **THEN** the partition table includes a partition of type `data`, subtype `spiffs`, named `storage`, with a size of at least 128KB

#### Scenario: Storage partition does not overlap with app partition
- **WHEN** the partition table is generated
- **THEN** the `storage` partition address range does not overlap with the `factory` (application) partition

### Requirement: SPIFFS is mounted at boot
The system SHALL mount the SPIFFS filesystem on the `storage` partition during startup, before the HTTP server starts.

#### Scenario: SPIFFS mounts successfully
- **WHEN** the system boots and the storage partition contains a valid SPIFFS image
- **THEN** SPIFFS is mounted at a well-known mount point (e.g., `/www`) and the mount succeeds

#### Scenario: Mount failure is handled gracefully
- **WHEN** the storage partition is empty or corrupted
- **THEN** the system logs an error and continues booting; the HTTP server starts but static file requests return 404

### Requirement: SPIFFS image is generated at build time
The build system SHALL generate a SPIFFS filesystem image from the `/data/www/` source directory and embed it in the firmware binary.

#### Scenario: Build creates SPIFFS image from data directory
- **WHEN** `platformio run` (or equivalent build command) is executed
- **THEN** a SPIFFS image is created from all files in `/data/www/` and included in the firmware binary

#### Scenario: Empty data directory produces valid image
- **WHEN** the `/data/www/` directory exists but is empty
- **THEN** the build succeeds and produces a valid (empty) SPIFFS image

#### Scenario: Build fails if SPIFFS image tool is missing
- **WHEN** the SPIFFS image generation tool is not found during the build
- **THEN** the build SHALL fail with a clear error message indicating the missing tool
