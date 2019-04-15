import os


with open('flowchart_test.txt','r') as f:
    for line in f:
       for word in line.split():
           #os.rename('train/' + word + '.xml', 'test/' + word + '.xml') 
           os.rename('train/' + word + '.jpg', 'test/' + word + '.jpg') 
