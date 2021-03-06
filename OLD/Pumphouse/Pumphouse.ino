// Wireless transmission/protocol stuff
#include <RF24Network.h>
#include <RF24.h>
#include <SPI.h>
#include <RF24Ethernet.h>
#include "RF24Mesh.h"

// Temp sensors
#include <OneWire.h>
#include <DallasTemperature.h>

// Watchdog
#include <avr/wdt.h>

// Define pin used by temperature sensors
#define ONE_WIRE_BUS 7
// Use high precision for temp sensors
#define TEMPERATURE_PRECISION 12 

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

// Number of temperature devices found
uint8_t numberOfOneWireDevices;

// Define known device addresses
DeviceAddress tempSensors[] = { { 0x28, 0xFF, 0xDD ,0x96,  0x36 , 0x16 , 0x03 , 0xB9 } ,{ 0x28,0xB3,0x8E,0x4A,0x07,0x00,0x00,0xA8 } ,{ 0x28,0x0A,0x62,0xB8,0x07,0x00,0x00,0xB4 } };

// Define friendly names for these addresses
char* tempSensorNames[] = { "Lake Water: ", "Outside Air: ", "Intake Pipe: " };

// Keep track of the readings from the devices
float temperatureReadings[3];

// How often to check wireless connection
#define MESH_CHECK_INTERVAL 15000

// HTTP request/response buffer sizes
#define HTTP_BUFFER_SIZE 400

// Initialize radio on pins 8 & 10 for CE and CS respectively
RF24 radio(8, 10);
// Define network, mesh, and IP layers
RF24Network network(radio);
RF24Mesh mesh(radio, network);
RF24EthernetClass RF24Ethernet(radio, network, mesh);
// Set server to run on port 1000
EthernetServer server = EthernetServer(1000);

// Timer used to check wireless connection status
uint32_t mesh_timer = 0;

// Pin on which an LED is connected so we can output connectivity status
#define LED_PIN 4

// Define error types
enum error {ERR_UNKNOWN, ERR_BUF_OVERFLOW, ERROR_NOT_TEMP_REQ, ERROR_NOT_GET_REQ};

/********************** Setup *********************/
void setup() {

	// In setup, disable WDT as sometimes things like initializing the radio may take a while
	wdt_disable();

	// Set our friendly "has connection" led to off
	pinMode(LED_PIN, OUTPUT);
	digitalWrite(LED_PIN, LOW);

	Serial.begin(115200);

	Serial.print(F("Initializing radio..."));

	// Set the IP address we'll be using. The last octet of the IP must be equal
	// to the designated mesh nodeID
	IPAddress myIP(10, 10, 2, 20);
	Ethernet.begin(myIP);
	mesh.setNodeID(20);

	// Init mesh
	if (!mesh.begin((uint8_t)97, RF24_250KBPS)) {
		Serial.println(F("Error: unable to intialize mesh; retrying..."));
		// If it fails, force reset arduino using watchdog -> enable w/ interval 0.5S, wait 1s
		wdt_enable(WDTO_500MS);
		delay(1000);
	}
	else {
		// If we're good, light up LED
		digitalWrite(LED_PIN, HIGH);
	}

	//Set IP of the RPi (gateway)
	IPAddress gwIP(10, 10, 2, 1);
	Ethernet.set_gateway(gwIP);
	// Init server
	server.begin();

	Serial.println(F("done."));
	Serial.println(F("Intializing temperature sensors..."));

	// Initialize OneWire temp sensors
	sensors.begin();

	// Grab a count of devices on the wire
	numberOfOneWireDevices = sensors.getDeviceCount();

	Serial.print(F("Found "));
	Serial.print(numberOfOneWireDevices, DEC);
	Serial.println(F(" OneWire devices."));

	// Loop through each device, print out address
	for (uint8_t i = 0; i < numberOfOneWireDevices; i++)
	{
		DeviceAddress tempDeviceAddress;
		// Search the wire for address
		if (sensors.getAddress(tempDeviceAddress, i))
		{
			Serial.print(F("Found device "));
			Serial.print(i, DEC);
			Serial.print(F(" with address: "));
			printAddress(tempDeviceAddress);
			Serial.println();

			// set the resolution to TEMPERATURE_PRECISION bit (Each Dallas/Maxim device is capable of several different resolutions)
			sensors.setResolution(tempDeviceAddress, TEMPERATURE_PRECISION);
			uint8_t actual_set = sensors.getResolution(tempDeviceAddress);
			if (actual_set != TEMPERATURE_PRECISION) {
				Serial.print(F("Error in setting temp precision; requested"));
				Serial.print(TEMPERATURE_PRECISION, DEC);
				Serial.print(" but received ");
				Serial.println(actual_set, DEC);
			}

		}
		else {
			Serial.print(F("Found ghost device at "));
			Serial.print(i, DEC);
			Serial.println(F(" but could not detect address. Check power and cabling"));
		}
	}

	// Setup done, enable watchdog
	wdt_enable(WDTO_8S);
}


// function to print a device address
void printAddress(DeviceAddress deviceAddress)
{
	for (uint8_t i = 0; i < 8; i++)
	{
		if (deviceAddress[i] < 16) Serial.print(F("0"));
		Serial.print(deviceAddress[i], HEX);
	}
}


// Saves temperature of a device to RAM
void saveTemperature(uint8_t sensorId)
{
	// Make sure sensor is connected
	if (sensors.isConnected(tempSensors[sensorId])) {
		float tempC = sensors.getTempC(tempSensors[sensorId]);
		// Correct the inside temperature sensor a bit
		if (sensorId == 2) {
			tempC -= 0.9375;
		}
		temperatureReadings[sensorId] = tempC;
		//Serial.print(tempSensorNames[sensorId]);
		//Serial.print(tempC);
		//Serial.println(F(" C"));
	}
	else {
		Serial.print(F("Error: unable to communicate with sensor "));
		Serial.println(tempSensorNames[sensorId]);
	}

}

/********************** Main Loop *********************/
void loop() {
	
	// Every MESH_CHECK_INTERVAL, check mesh connection
	if (millis() - mesh_timer > MESH_CHECK_INTERVAL) {
		mesh_timer = millis();
		if (!mesh.checkConnection()) {
			// If fail, indicate with LED and renew
			digitalWrite(LED_PIN, LOW);
			mesh.renewAddress();
			digitalWrite(LED_PIN, HIGH);
		}
		else {
			digitalWrite(LED_PIN, HIGH);
		}
	}

	// If message available (that is, a TCP connection to our server port)
	if (EthernetClient client = server.available())
	{
		bool transmit = false;
		bool evalInput = true;
		// Use error type to formulate appropriate response
		error error_type = ERR_UNKNOWN;

		// Define buffer and index to it
		int idx = 0;
		char buf[HTTP_BUFFER_SIZE];

		// Read data from client
		while (int dataToRead = client.waitAvailable() > 0) {
			// When reading data, make sure to only read appropriate amount to avoid buffer overflow
			if (dataToRead <= HTTP_BUFFER_SIZE - idx) {
				idx += client.read((uint8_t*)&buf + idx, dataToRead);
			}
			else {
				evalInput = false;
				error_type = ERR_BUF_OVERFLOW;
				break;
			}
		}

		// Clear out anything left
		// Seems sometimes if it is not cleared transmitting can have problems
		client.flush();

		if (evalInput) {
			// Search for a GET request
			char* gettype = strstr(buf, "GET");

			// If GET request
			if (gettype != NULL) {
				// Split by space (twice)
				strsep(&gettype, " ");
				char* addr = strsep(&gettype, " ");
				// After splitting, check the requested file: must be /temp
				if (strcmp(addr, "/temp") == 0) {
					// All OK: transmit
					transmit = true;
				}
				else {
					error_type = ERROR_NOT_TEMP_REQ;
				}
			}
			else {
				error_type = ERROR_NOT_GET_REQ;
			}
		}


		if (transmit) {
			// Tell all sensors to update temperatures
			sensors.requestTemperatures();

			Serial.println(F("Temperature request received; responding..."));

			// Define output buffer incl. HTTP response header
			// Overwrite input buffer since we don't need it anymore
			strcpy(buf, "HTTP/1.1 200 OK \r\nContent-Type: text/plain \r\nConnection: close \r\nRefresh: 5 \r\n\r\n");

			// Go through all sensors and send
			for (uint8_t i = 0; i < numberOfOneWireDevices; i++) {
				// Update temperature arrray
				saveTemperature(i);

				// Add sensor name to response
				strcat(buf, tempSensorNames[i]);

				// Convert temp float to string and save to buffer
				char fltStr[10];
				dtostrf(temperatureReadings[i], 0, 5, fltStr);

				strcat(buf, fltStr);

				// Add a | if necessary (i.e. more sensors to follow)
				if (i < numberOfOneWireDevices - 1) {
					strcat(buf, "|");
				}

			}
			// Add final line ending
			strcat(buf, "\r\n");
			// Send to client
			client.write(buf, strlen(buf));
			// Close connection
			client.stop();

		}
		else {
			//Error: respond with appropriate message
			switch ( error_type)
			{
			case ERR_BUF_OVERFLOW:
				client.write("HTTP/1.1 431 Request Header Fields Too Large \r\n\r\n");
				break;
			case ERROR_NOT_GET_REQ:
				client.write("HTTP/1.1 405 Method Not Allowed \r\n\r\n");
				break;
			case ERROR_NOT_TEMP_REQ:
				client.write("HTTP/1.1 501 Not Implemented \r\n\r\nOnly temperature(/temp) requests supported.\r\n");
				break;
			case ERR_UNKNOWN:
			default:
				client.write("HTTP/1.1 520 Unknown Error \r\n\r\n");
				break;
			}
			client.stop();
		}
	}

	// If our loop continues executing (and no readio failure), tell watchdog we're OK
	if (!radio.failureDetected) wdt_reset();

}
