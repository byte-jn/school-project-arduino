# Liest die Log-Zeilen eines Arduino Uno (Sensor oder Lüfter) vom COM-Port, ergänzt Datum/Uhrzeit
# und hängt sie an eine CSV-Datei an.
#
# Aufruf (Serial Monitor der Arduino IDE vorher schließen, der Port kann nur einmal offen sein):
#   powershell -ExecutionPolicy Bypass -File .\log-serial.ps1 -Port COM7 -File luefter.csv
#
# Für beide Unos gleichzeitig: zweimal starten, je mit eigenem -Port und -File.
#
# Beenden mit Strg+C.

param(
    [string]$Port = "COM7",
    [int]$Baud = 9600,
    [string]$File = "log.csv"
)

if (-not (Test-Path $File)) {
    Add-Content -Path $File -Value "datum;zeit_s;geraet;ereignis;wert;detail" -Encoding UTF8
}

$serial = New-Object System.IO.Ports.SerialPort $Port, $Baud
$serial.NewLine = "`n"
$serial.Open()
$serial.DiscardInBuffer()
Write-Host "Logge $Port ($Baud Baud) nach $File - Strg+C beendet."

try {
    # Die erste Zeile nach dem Öffnen kann abgeschnitten sein, deshalb verwerfen.
    [void]$serial.ReadLine()

    while ($true) {
        $line = $serial.ReadLine().Trim()
        if ($line -eq "" -or $line -like "zeit_s;*") { continue }   # Leerzeilen und Arduino-Kopfzeile

        $out = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss');$line"

        Write-Host $out
        Add-Content -Path $File -Value $out -Encoding UTF8
    }
}
finally {
    $serial.Close()
}
