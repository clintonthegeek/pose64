# ReControl Integration Tests

Integration tests for the ReControl TCP remote control interface.

## Overview

The `test_recontrol.py` script provides comprehensive testing of the ReControl
TCP server, including:

- **Happy Path Tests**: Normal command sequences and functionality
- **Error Handling**: Invalid commands, connection rejections
- **Concurrency**: Multi-connection rejection enforcement
- **Robustness**: Disconnect/reconnect scenarios

## Prerequisites

- POSE64 emulator built and ready
- Python 3.6+
- ReControl server listening on port 6425 (default)

## Running Tests

### Start the emulator with ReControl enabled:

```bash
./pose64 --port 6425
```

### Run the test suite:

```bash
python3 test_recontrol.py
```

### Options:

```bash
python3 test_recontrol.py --help
python3 test_recontrol.py --host localhost --port 6425 --timeout 5
```

## Test Coverage

### Test 1: Basic Connection and Info
- Connects to ReControl server
- Validates info command response
- Checks version and device information

### Test 2: State Command
- Verifies state command returns valid status
- Tests response with suspend counter diagnostics
- Validates state values: running, suspended, stopped, blocked_on_ui

### Test 3: Invalid Command Handling
- Tests that invalid commands produce ERR responses
- Verifies error message format

### Test 4: Multi-Connection Rejection
- Attempts to establish multiple simultaneous connections
- Verifies that second connection is rejected
- Ensures only one client connection at a time

### Test 5: Disconnect and Reconnect
- Establishes connection
- Disconnects
- Re-establishes connection
- Verifies both connections work correctly

### Test 6: Screenshot Command
- Tests screenshot capture functionality
- Validates response format

### Test 7: Input Commands
- Tests tap command (pen input)
- Tests key command (keyboard input)
- Tests button command (device buttons)

### Test 8: Command Sequence
- Tests multiple commands in sequence
- Verifies proper state management between commands

## Expected Output

```
ReControl Integration Tests
Connecting to localhost:6425

=== Test 1: Basic Connection and Info ===
PASS: Info command response: OK POSE64 0.9.0
...

============================================================
Tests Passed: 8/8
  basic_connection, state_command, invalid_command, multi_connection, disconnect_reconnect, screenshot_command, input_commands, command_sequence
```

## Troubleshooting

### Connection Failed
- Ensure POSE64 is running: `ps aux | grep pose64`
- Check port is listening: `netstat -tlnp | grep 6425`
- Verify firewall isn't blocking connections

### Test Timeout
- Increase timeout: `--timeout 10`
- Check if emulator is responsive
- Review emulator logs for errors

### Multi-Connection Test
- The test may succeed at TCP level (connection accepted but commands rejected)
- Or fail at socket level (connection refused)
- Both behaviors indicate proper multi-connection rejection

## Protocol Documentation

See ReControl.h for TCP protocol specification:
- Command format: `command arg1 arg2 ...\n`
- Response format: `OK response_data\n` or `ERR category message\n`
- Multi-line responses: Terminated by single `.` on final line

## Exit Status

- 0: All tests passed
- 1: One or more tests failed
