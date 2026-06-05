# Feature: Daten-Integrität & Fail-Safe Mechanismen

## 1. Zielsetzung
Sicherstellung der Datenkonsistenz und Systemstabilität auch bei Hardware-Ausfällen oder extremen Lastsituationen.

## 2. Technische Anforderungen
- **Atomares Logging**: Sicherstellung, dass Log-Dateien bei Systemabsturz nicht korrumpiert werden (z.B. durch Write-Ahead-Logging oder regelmäßiges `fsync`).
- **Heartbeat-System**: Überwachung der Kernmodule (Camera, Tracker, Logger) durch einen Watchdog-Prozess.
- **Sensor-Failover**: Definierte Zustände bei Signalverlust (z.B. "Last Known Position" Extrapolation vs. sofortiger Alert).
- **Data Checksums**: Generierung von Prüfsummen (SHA-256) für exportierte Datensätze zur Verifizierung der Integrität nach dem Export.

## 3. UI-Integration
- Globale Status-Bar für Modul-Health (Grün/Gelb/Rot).
- Kritische Warn-Overlays bei Dateninkonsistenz oder Puffer-Überlauf.

## 4. Validierung
- Stress-Tests: Simulation von plötzlichem Kamera-Disconnect während aktivem Logging.
- Korruptions-Test: Manuelles Unterbrechen der Stromzufuhr (falls Hardware-nah) und Prüfung der Log-Integrität.
