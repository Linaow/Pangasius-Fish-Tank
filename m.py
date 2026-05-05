"""
fish_detector.py
----------------
Runs YOLOv8 fish detection on a USB camera and streams
detection results to the Node.js dashboard via HTTP POST.

Install:
    pip install ultralytics opencv-python requests

Run:
    python fish_detector.py
"""

import cv2
import requests
import time
from ultralytics import YOLO

# ── Settings ───────────────────────────────────────────
MODEL_PATH    = "best.pt"
CONFIDENCE    = 0.4          # default fallback for unlisted species
SPECIES_CONF  = {
    "tilapia":   0.25,       # lowered
    "zebra":     0.25,       # lowered
    "pangasius": 0.6,        # raised
}
NODE_URL      = "http://localhost:3000/detections"
SEND_INTERVAL = 0.1   # seconds between sends (~10fps to dashboard)
TOP_ZONE      = 0.33
BOTTOM_ZONE   = 0.66
# Set this to your Rapoo camera index if known.
# If None, the script scans available cameras automatically.
CAMERA_INDEX  = 1
# ───────────────────────────────────────────────────────


def find_usb_camera():
    """Open the chosen camera index or scan indices 0-5 for a working camera."""
    if CAMERA_INDEX is not None:
        print(f"Trying camera index {CAMERA_INDEX}...")
        cap = cv2.VideoCapture(CAMERA_INDEX, cv2.CAP_DSHOW)
        if cap.isOpened():
            ret, _ = cap.read()
            if ret:
                print(f"✅ Camera opened at index {CAMERA_INDEX}")
                return cap
            cap.release()
        print(f"⚠️ Camera index {CAMERA_INDEX} did not return frames. Please update CAMERA_INDEX to your Rapoo camera index.")
        return None

    for index in range(6):
        cap = cv2.VideoCapture(index, cv2.CAP_DSHOW)
        if cap.isOpened():
            ret, _ = cap.read()
            if ret:
                print(f"✅ Camera found at index {index}")
                return cap
            cap.release()
    for index in range(6):
        cap = cv2.VideoCapture(index)
        if cap.isOpened():
            ret, _ = cap.read()
            if ret:
                print(f"✅ Camera found at index {index}")
                return cap
            cap.release()
    return None


def get_position(y_center, frame_height):
    r = y_center / frame_height
    if r < TOP_ZONE:
        return "Top"
    elif r < BOTTOM_ZONE:
        return "Middle"
    return "Bottom"


def send_to_node(payload):
    try:
        requests.post(NODE_URL, json=payload, timeout=0.5)
    except Exception:
        pass  # dashboard offline, keep detecting anyway


def main():
    print("Loading model...")
    model = YOLO(MODEL_PATH)
    print(f"Classes: {model.names}\n")

    cap = find_usb_camera()
    if cap is None:
        print("No camera found. Check USB connection.")
        return

    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

    last_send = 0
    print("Running — press Q to quit")

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        h, w = frame.shape[:2]
        results = model(frame, conf=min(SPECIES_CONF.values(), default=CONFIDENCE), verbose=False)[0]

        detections = []
        for box in results.boxes:
            cls_id     = int(box.cls[0])
            confidence = float(box.conf[0])
            name       = model.names[cls_id]

            # Apply per-species confidence threshold
            if confidence < SPECIES_CONF.get(name, CONFIDENCE):
                continue

            x1, y1, x2, y2 = map(int, box.xyxy[0])
            y_center   = (y1 + y2) // 2
            position   = get_position(y_center, h)

            detections.append({
                "species":    name,
                "position":   position,
                "confidence": round(confidence, 2),
                "box": {"x1": x1, "y1": y1, "x2": x2, "y2": y2}
            })

            color = {"Top": (0,255,0), "Middle": (0,165,255), "Bottom": (0,0,255)}[position]
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
            label = f"{name} | {position} {confidence:.0%}"
            cv2.putText(frame, label, (x1, y1 - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2, cv2.LINE_AA)

        now = time.time()
        if now - last_send >= SEND_INTERVAL:
            send_to_node({
                "timestamp":  int(now * 1000),
                "total":      len(detections),
                "detections": detections
            })
            last_send = now

        cv2.imshow("Fish Detector [Q to quit]", frame)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()