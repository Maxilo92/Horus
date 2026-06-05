# Feature: Daten-Logging & Export

## 1. Zielsetzung
Automatisierte Erfassung und Speicherung aller Tracking-Daten zur nachgelagerten Analyse.

## 2. Technische Anforderungen
- **Export-Formate**: CSV und JSON (maschinenlesbar).
- **Datenfelder**: Zeitstempel (ms), Objekt-ID, X/Y (px), X/Y (real), Breite/Höhe, Geschwindigkeit.
- **Logging-Frequenz**: Einstellbar (z.B. pro Frame oder alle X ms).

## 3. UI-Integration
- "Start/Stop Logging" Button.
- Anzeige des Pfades zur aktuellen Log-Datei.

## 4. Validierung
- Import der generierten CSV in Excel/Pandas zur Plausibilitätsprüfung.
