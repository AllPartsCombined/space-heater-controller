# space-heater-controller
Arduino code for accessing ecobee api to control a space heater with a relay. There is also HTML code for hosting a web server for inputting settings and reading logs. 
It is based on a NodeMCU ESP8266 microcontroller. 

Unzip the Data folder in the same directory as the Arduino code. Install the ESP8266 Sketch Data Upload tool, and use it to upload the contents of the data folder to memory on the microcontroller.
Make sure to edit the SSID, Password, and AppID variables, then upload the sketch. 
Full tutorial, including hardware set up, can be found at https://youtu.be/MgeW2lPQgPg
