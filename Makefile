CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17
INCLUDES = -Iinclude
LIBS = -lz

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = .

SOURCES = $(SRC_DIR)/main.cpp $(SRC_DIR)/portfolio_data.cpp $(SRC_DIR)/file_utils.cpp $(SRC_DIR)/web_server.cpp $(SRC_DIR)/market_data_sync.cpp $(SRC_DIR)/plaid_client.cpp
OBJECTS = $(OBJ_DIR)/main.o $(OBJ_DIR)/portfolio_data.o $(OBJ_DIR)/file_utils.o $(OBJ_DIR)/web_server.o $(OBJ_DIR)/market_data_sync.o $(OBJ_DIR)/plaid_client.o
TARGET = $(BIN_DIR)/finance_tracker
TEST_PERSISTENCE_BIN = $(BIN_DIR)/test_persistence

.PHONY: all clean run test-persistence

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

$(TEST_PERSISTENCE_BIN): test_persistence.cpp $(SRC_DIR)/portfolio_data.cpp $(SRC_DIR)/file_utils.cpp $(SRC_DIR)/market_data_sync.cpp $(SRC_DIR)/web_server.cpp $(SRC_DIR)/plaid_client.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

test-persistence: $(TEST_PERSISTENCE_BIN)
	./$(TEST_PERSISTENCE_BIN) --data-dir data

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TEST_PERSISTENCE_BIN)
