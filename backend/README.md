cd backend && CGO_ENABLED=0 go build -o bin/server ./cmd/server

curl http://localhost:8080/v1/device/test-device-001/next
curl http://localhost:8080/nfc/test-device-001
curl http://localhost:8080/metrics