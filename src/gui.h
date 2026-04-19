#pragma once

void RunGUI();
void LoadLogFile();
void ColoredIndicator(const char* label, bool condition, 
                      const char* true_text = "ON", const char* false_text = "OFF");