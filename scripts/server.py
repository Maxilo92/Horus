from fastapi import FastAPI, File, UploadFile
from ultralytics import YOLO
import uvicorn
import cv2
import numpy as np

app = FastAPI(title="Project Horus Remote Inference Server")

# Load YOLOv8 model on GPU (CUDA)
# It will download 'yolov8m.pt' (Medium model) if not present locally.
# YOLOv8m has significantly higher accuracy and recall than YOLOv8n.
print("Loading YOLOv8m model on CUDA...")
try:
    model = YOLO("yolov8m.pt").to("cuda")
    print("YOLOv8m loaded on GPU (CUDA) successfully.")
except Exception as e:
    print(f"Failed to load YOLOv8m on CUDA: {e}")
    print("Falling back to CPU...")
    model = YOLO("yolov8m.pt")

@app.post("/detect")
async def detect_objects(file: UploadFile = File(...)):
    # Read uploaded image bytes
    contents = await file.read()
    nparr = np.frombuffer(contents, np.uint8)
    img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    
    if img is None:
        return {"detections": []}
    
    # Run YOLOv8 prediction on GPU
    # conf=0.25 removes low-confidence misclassifications (like houses as planes)
    results = model.predict(img, imgsz=640, conf=0.25, verbose=False)
    
    detections = []
    for r in results:
        boxes = r.boxes
        for box in boxes:
            b = box.xyxy[0].cpu().numpy() # x1, y1, x2, y2
            cls = int(box.cls[0].cpu().item())
            conf = float(box.conf[0].cpu().item())
            
            # Form standard x, y, w, h bounding box format
            detections.append({
                "class_id": cls,
                "confidence": conf,
                "box": [
                    int(b[0]),                   # left
                    int(b[1]),                   # top
                    int(b[2] - b[0]),            # width
                    int(b[3] - b[1])             # height
                ]
            })
            
    return {"detections": detections}

@app.get("/health")
async def health_check():
    return {"status": "ok", "device": str(model.device)}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
