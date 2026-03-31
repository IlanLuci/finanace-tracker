CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17
INCLUDES = -Iinclude

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = .

SOURCES = $(SRC_DIR)/main.cpp $(SRC_DIR)/portfolio_data.cpp $(SRC_DIR)/file_utils.cpp $(SRC_DIR)/web_server.cpp
OBJECTS = $(OBJ_DIR)/main.o $(OBJ_DIR)/portfolio_data.o $(OBJ_DIR)/file_utils.o $(OBJ_DIR)/web_server.o
TARGET = $(BIN_DIR)/finance_tracker

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
