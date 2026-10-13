# flash.ps1 — Script para gravar o firmware no ESP32
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Gravando Firmware Aurora no ESP32 (COM3)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "DICA: Se aparecer 'Connecting........_____....._____'," -ForegroundColor Yellow
Write-Host "mantenha pressionado o botao 'BOOT' (ou '0') na sua placa ESP32" -ForegroundColor Yellow
Write-Host "por 2 segundos ate comecar a porcentagem de gravacao!`n" -ForegroundColor Yellow

& $pio run -t upload

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nFirmware gravado com SUCESSO!" -ForegroundColor Green
    Write-Host "Abrindo Serial Monitor (pressione Ctrl+C para sair)..." -ForegroundColor Cyan
    & $pio device monitor
} else {
    Write-Host "`nFalha na gravacao. Lembre-se de segurar o botao BOOT enquanto o esptool tenta conectar." -ForegroundColor Red
}
