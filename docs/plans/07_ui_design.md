# UI-Design-Spezifikation: Project Horus (Tactileviewer)

## 1. Design-Philosophie
Das UI ist als **funktionales Kontrollzentrum** konzipiert. Der Fokus liegt auf maximaler Datendichte, Lesbarkeit unter verschiedenen Lichtverhältnissen und schnellem Zugriff auf Analyse-Parameter.

## 2. Layout-Struktur (Workspace)
Die Benutzeroberfläche wird in einem "Dashboard"-Layout organisiert, wobei ImGui-Docking genutzt wird, um Fenster flexibel anzuordnen.

### 2.1 Main Viewport (Zentrum)
- **Inhalt**: Der Live-Video-Feed der Kamera.
- **Overlays**: Bounding Boxes, IDs und Koordinaten (High-Contrast, schmale Linien).
- **Interaktion**: Direktes Zeichnen von ROIs oder Selektieren von Objekten per Mausklick im Stream.

### 2.2 Control Panel (Rechts)
- **Tracking Settings**: Slider für Confidence-Threshold, IOU-Schwellenwerte und Filter-Parameter.
- **Calibration**: Controls zum Starten des Kalibrierungsprozesses und Eingabe realer Maße.
- **ROI Editor**: Liste der aktiven Zonen mit Schaltflächen zum Aktivieren/Deaktivieren.

### 2.3 Data Panel (Unten)
- **Live-Tabelle**: Tabellarische Auflistung aller aktuell getrackten Objekte.
- **Spalten**: ID, Klasse, Position (X,Y), Geschwindigkeit (v), Status (Erfasst/Verloren).
- **Export-Steuerung**: Buttons für "Start/Stop Logging" und Dateipfadauswahl.

### 2.4 Diagnostics Panel (Links)
- **System-Telemetrie**: Graphen für FPS-Stabilität und Latenzverlauf.
- **Signal-Status**: Anzeige der Verbindungsqualität und Frame-Drop-Statistiken.
- **Hardware-Auslastung**: CPU/GPU-Bar-Indikatoren.

### 2.5 Menu Bar (Oben)
- **File**: Profile speichern/laden (Kalibrierungs-Setups).
- **View**: Fenster ein-/ausblenden (Tabelle, Telemetrie).
- **Tools**: Zugriff auf Screenshot-Funktion und Rohdaten-Viewer.

## 3. Visuelle Standards
- **Farbpalette**: Dunkles Theme (Grey/Black) mit Akzentfarben für Datenzustände:
  - **Cyan**: Standard-Daten/Ziele.
  - **Grün**: System OK / Kalibrierung abgeschlossen.
  - **Gelb/Orange**: Warnungen / Hohe Latenz.
  - **Rot**: Fehler / Signalverlust.
- **Typografie**: Ausschließlich Monospaced-Fonts (z.B. *Roboto Mono*) für alle Zahlenwerte, um Spaltenausrichtung zu gewährleisten.
- **Icons**: Reduzierte, technische Icons für Funktionen wie Speichern, Löschen oder Aufnehmen.

## 4. Interaktionsmodell
- **Modus-Umschaltung**: Klare Trennung zwischen "Monitoring-Modus" (Analyse läuft) und "Setup-Modus" (Kalibrierung/ROI-Bearbeitung).
- **Keyboard-Shortcuts**: 
  - `Space`: Logging Start/Stop.
  - `C`: Kalibrierung öffnen.
  - `Esc`: Modus abbrechen / Menü schließen.

## 5. Validierung
- Prüfung der Bedienbarkeit mit Maus und Tastatur.
- Sicherstellung, dass die UI-Overlays den Video-Stream nicht durch zu dicke Linien oder große Fenster verdecken.
