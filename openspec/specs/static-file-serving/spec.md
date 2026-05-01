## ADDED Requirements

### Requirement: HTTP server serves static files from SPIFFS
The HTTP server SHALL serve HTML, CSS, and JavaScript files stored on a SPIFFS filesystem in response to HTTP GET requests.

#### Scenario: Serve HTML page from SPIFFS
- **WHEN** a client requests `GET /` 
- **THEN** the server reads the corresponding HTML file from SPIFFS and returns it with `Content-Type: text/html` and HTTP 200

#### Scenario: Serve CSS file from SPIFFS
- **WHEN** a client requests `GET /styles.css`
- **THEN** the server reads `styles.css` from SPIFFS and returns it with `Content-Type: text/css` and HTTP 200

#### Scenario: Serve JavaScript file from SPIFFS
- **WHEN** a client requests `GET /script.js`
- **THEN** the server reads `script.js` from SPIFFS and returns it with `Content-Type: application/javascript` and HTTP 200

#### Scenario: File not found returns 404
- **WHEN** a client requests a file path that does not exist on the SPIFFS filesystem
- **THEN** the server SHALL return HTTP 404 with a plain-text error message

#### Scenario: Content-Type is determined by file extension
- **WHEN** the server reads a file from SPIFFS
- **THEN** the server SHALL set the `Content-Type` header based on the file extension (`.html` → `text/html`, `.css` → `text/css`, `.js` → `application/javascript`, others → `application/octet-stream`)

### Requirement: Dynamic API endpoints remain unchanged
The existing API endpoints (`/api/status`, `/save`, `/reconnect`, `/reset`, `/config`) SHALL continue to function identically, generating responses from C code without relying on the filesystem.

#### Scenario: API status endpoint returns JSON
- **WHEN** a client requests `GET /api/status`
- **THEN** the server returns a JSON response with device status data, identical to the current behavior

#### Scenario: Save endpoint processes form data
- **WHEN** a client submits `POST /save` with WiFi credentials
- **THEN** the server saves credentials to NVS and returns a success page, identical to the current behavior

### Requirement: Static web UI files are stored in project source tree
All static web assets SHALL be stored as plain files under `/data/www/` in the project root, organized by type.

#### Scenario: HTML files are in data directory
- **WHEN** a developer looks at the project source
- **THEN** all HTML page files are in `/data/www/` with `.html` extensions

#### Scenario: CSS files are in data directory
- **WHEN** a developer looks at the project source
- **THEN** all CSS style files are in `/data/www/` with `.css` extensions

#### Scenario: JavaScript files are in data directory
- **WHEN** a developer looks at the project source
- **THEN** all JavaScript files are in `/data/www/` with `.js` extensions
