int min = 1000;
int max = 3000;
int start = 0;
int end = 0;
int diff = 0;

void setup()
{
  pinMode(13, OUTPUT);
  pinMode(12, INPUT_PULLUP);
  Serial.begin(9600);
  randomSeed(millis()); 
}

void loop()
{
  if (digitalRead(13) == LOW) {
    delay(random(min, max));
    start = millis();
    digitalWrite(13, HIGH);
  }
  if (digitalRead(12) == LOW) {
    end = millis();
    diff = end - start;
    Serial.print(diff);
    Serial.println(" ms");
    digitalWrite(13, LOW);
  }
}