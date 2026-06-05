# Feature: Region of Interest (ROI) Management

## 1. Zielsetzung
Einschränkung der Analyse auf definierte Bildbereiche zur Performance-Optimierung und Filterung irrelevanter Bewegungen.

## 2. Technische Anforderungen
- **Definition**: Definition von rechteckigen Zonen per Drag & Drop im HUD.
- **Processing**: Übergabe der ROI-Koordinaten an die Detection-Engine zur selektiven Verarbeitung.
- **Multi-ROI Support**: Unterstützung von bis zu 4 unabhängigen Überwachungszonen.

## 3. UI-Integration
- ROI-Editormodus zum Zeichnen und Verschieben der Zonen.
- Visuelle Hervorhebung aktiver ROIs im HUD.

## 4. Validierung
- Benchmarking der FPS-Steigerung bei Nutzung einer kleinen ROI vs. Vollbild.
