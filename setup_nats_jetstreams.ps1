# Konfiguration
$NatsPath   = "C:\Apps\natsclient\nats.exe"
$NatsServer = "192.168.1.254:4222"
$StreamName = "sensors"
$Subjects   = "sensors.>"      # Fångar sensors.lsm6dsox.raw, sensors.bme280.raw och kommande sensorer
$PerSubject = 100000           # Max meddelanden per subject (IMU @5 Hz ~5,5 h, BME280 @0,1 Hz ~11 dygn)

Write-Host "=== Konfigurerar gemensam JetStream-stream '$StreamName' ===" -ForegroundColor Cyan
Write-Host "Ansluter till: $NatsServer" -ForegroundColor Yellow

# 1. Kontrollera anslutning
Write-Host "`n[1/3] Kontrollerar serverstatus..." -ForegroundColor White
& $NatsPath --server=$NatsServer rtt
if ($LASTEXITCODE -ne 0) {
    Write-Error "Kunde inte nå NATS-servern på $NatsServer."
    exit 1
}

# 2. Skapa streamen om den saknas, annars uppdatera subjects och gränser.
#    Gränsen per subject gör att IMU-datat (5 Hz) inte tränger ut barometerhistoriken (var 10:e s).
Write-Host "`n[2/3] Skapar/uppdaterar stream: $StreamName..." -ForegroundColor White
& $NatsPath --server=$NatsServer stream info $StreamName *> $null
if ($LASTEXITCODE -ne 0) {
    & $NatsPath --server=$NatsServer stream add $StreamName `
        --subjects=$Subjects `
        --storage=file `
        --retention=limits `
        --max-msgs=-1 `
        --max-msgs-per-subject=$PerSubject `
        --max-age=7d `
        --dupe-window=2m `
        --defaults
}
else {
    & $NatsPath --server=$NatsServer stream edit $StreamName `
        --subjects=$Subjects `
        --max-msgs=-1 `
        --max-msgs-per-subject=$PerSubject `
        --force
}

# 3. Verifiera
Write-Host "`n[3/3] Verifierar..." -ForegroundColor White
& $NatsPath --server=$NatsServer stream info $StreamName
& $NatsPath --server=$NatsServer stream subjects $StreamName

Write-Host "`n=== Klart ===" -ForegroundColor Cyan
Write-Host "Följ live-data med:" -ForegroundColor Gray
Write-Host "  & '$NatsPath' --server=$NatsServer sub 'sensors.>'" -ForegroundColor Yellow
