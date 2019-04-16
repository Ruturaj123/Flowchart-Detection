# Flowchart-Detection
Detecting hand drawn flowcharts using Tensorflow Object Detection API


This repository uses Tensorflow Object Detecttion API for detecting hand drawn flowcharts.
The dataset images are located in the folder `models/research/object_detection/images/test`


I have used the Faster-RCNN Inceptionv2 model for detecting the flowcharts.

### Output

![Output](flowchart.png?raw=true)


### Running Locally
* Install the [Tensorflow Object Detection API](https://github.com/tensorflow/models/blob/master/research/object_detection/g3doc/installation.md)

* ``` bash
  #From Flowchart-Detection/models
  export PYTHONPATH=$PYTHONPATH:`pwd`:`pwd`/research:`pwd`/research/slim:`pwd`/research/object_detection
  ```

* ``` bash 
  #From Flowchart-Detection/
  python Object_detection_image.py        (#You can set any input image of your choice located in models/research/object_detection/images/test inside this script)
  ```
* Coordinates of the bounding boxes will be stores in coordinates.json

**NOTE:** OpenCV is needed for displaying the image. It can be installed using `pip install opencv-python`
