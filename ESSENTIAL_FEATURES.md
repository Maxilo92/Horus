# Essenzielle Funktionen: Project Horus (Tactileviewer) - Technisches Tool-Profil

Dieses Dokument spezifiziert die rein funktionalen Kernanforderungen für Project Horus als technisches Analyse- und Tracking-Werkzeug. Alle ästhetischen oder atmosphärischen Elemente wurden zugunsten von Präzision, Zuverlässigkeit und Datenintegrität entfernt.

## 1. Präzisions-Tracking & Bildverarbeitung (Technical Core)
- **Stabile Multi-Objekt-Erfassung**: Implementierung von robusten Detektions-Algorithmen mit Fokus auf geringe Latenz und hohe Wiederholgenauigkeit.
- **Eindeutige Identifizierung (UUID)**: Jedem erfassten Objekt muss eine persistente ID zugewiesen werden, um Bewegungsabläufe über lange Zeiträume ohne Verwechslung zu tracken.
- **Kalibrierung & Skalierung**: Werkzeuge zur Kalibrierung der Pixelmaße auf reale physikalische Einheiten (z.B. mm oder m) basierend auf Referenzobjekten im Sichtfeld.
- **Region of Interest (ROI)**: Definition von Überwachungsbereichen zur Reduktion der CPU-Last und Erhöhung der Rechengeschwindigkeit in kritischen Zonen.

## 2. Datenvisualisierung & Interface (Utility UI)
- **High-Contrast Overlays**: Klare, schnörkellose Darstellung von Begrenzungsrahmen (Bounding Boxes) und Koordinatenkreuzen ohne visuelles Rauschen.
- **Echtzeit-Koordinatenanzeige**: Einblendung der exakten X/Y-Position und Objektgröße direkt am Ziel oder in einer tabellarischen Übersicht.
- **Vektoranalyse**: Anzeige der Bewegungsrichtung und Geschwindigkeit (px/s oder m/s) als präzise mathematische Vektoren.
- **System-Telemetrie**: Permanente Überwachung von Hardware-Parametern (CPU/GPU-Last, Video-Latenz, Frame-Drop-Rate).

## 3. Datenmanagement & Persistenz (Data Integrity)
- **Ereignis-Logging**: Automatisches Protokollieren von Detektionen, Positionsänderungen und Systemereignissen in maschinenlesbare Formate (CSV, JSON, SQL).
- **Session-Recording**: Aufzeichnung des Roh-Video-Feeds parallel zu den Metadaten für spätere Offline-Analysen.
- **Snapshot-Funktion**: Speichern von Standbildern bei vordefinierten Triggern (z.B. Eintritt eines Objekts in eine ROI).

## 4. Steuerung & Systemintegration (Control)
- **Parameter-Konfiguration**: Feinjustierung von Schwellenwerten (Confidence, IOU, Filter-Parameter) über ein dediziertes Bedienpanel während der Laufzeit.
- **Schnittstellen-Vorbereitung**: Export der Tracking-Daten in Echtzeit für die Verarbeitung durch externe Systeme.
- **Input-Management**: Unterstützung verschiedener professioneller Videoquellen (IP-Kameras, USB3-Vision, High-Speed-Kameras).

## 5. Zuverlässigkeit & Fehlerbehandlung
- **Signalverlust-Handling**: Definierte Prozeduren und Warnmeldungen bei Unterbrechung des Video-Feeds oder Hardware-Fehlern.
- **Integritätsprüfung**: Validierung der Detektionsergebnisse zur Minimierung von "False Positives" durch statistische Filterung.
