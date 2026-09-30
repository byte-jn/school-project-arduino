int analogPin = A3; // Define the analog pin to read from
int red = 13;
int yellow = 12;
int blue = 11;
int val = 0;

void setup() {
  Serial.begin(9600); // Initialize serial communication at 9600 baud rate
  pinMode(analogPin, INPUT);
  pinMode(red, OUTPUT);
  pinMode(yellow, OUTPUT);
  pinMode(blue, OUTPUT);
}

void loop() {
  // Read the analog value from the specified pin
  val = analogRead(analogPin);

  // Control the LEDs based on the analog value read
  if (val <= 340) {
    digitalWrite(red, HIGH);
    digitalWrite(yellow, LOW);
    digitalWrite(blue, LOW);
  } else if (val <= 682) {
    digitalWrite(red, LOW);
    digitalWrite(yellow, HIGH);
    digitalWrite(blue, LOW);
  } else {
    digitalWrite(red, LOW);
    digitalWrite(yellow, LOW);
    digitalWrite(blue, HIGH);
  }

  // Print the analog value to the serial monitor
  Serial.print("Analog value: ");
  Serial.println(val);

  delay(10);
}
