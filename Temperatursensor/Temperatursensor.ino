#include <DHT.h>

#define DHTPIN 2
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

float warnTemp = 25.0;
float badTemp = 26.0;

void setup()
{
  pinMode(13, OUTPUT);
  pinMode(12, OUTPUT);
  pinMode(11, OUTPUT);

  Serial.begin(9600);
  dht.begin();
}

void loop()
{
  float currentTemp = dht.readTemperature();

  Serial.print("Temperatur: ");
  Serial.print(currentTemp);
  Serial.println(" °C");

  if (currentTemp > badTemp)
  {
    digitalWrite(13, HIGH);
    digitalWrite(12, LOW);
    digitalWrite(11, LOW);
  }
  else if (currentTemp > warnTemp)
  {
    digitalWrite(13, LOW);
    digitalWrite(12, HIGH);
    digitalWrite(11, LOW);
  }
  else
  {
    digitalWrite(13, LOW);
    digitalWrite(12, LOW);
    digitalWrite(11, HIGH);
  }

  delay(500);
}