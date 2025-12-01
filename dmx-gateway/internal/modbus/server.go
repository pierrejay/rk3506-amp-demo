// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package modbus

import (
	"encoding/binary"
	"log/slog"
	"sync"

	"github.com/tbrandon/mbserver"

	"dmx-gateway/internal/dmx"
)

// Config for Modbus TCP server
type Config struct {
	Port string `yaml:"port"` // ":502" or ":5020"
}

// Server is the Modbus TCP server for DMX gateway
// Register mapping:
//   - Holding registers 0-511 = DMX channels 1-512 (value 0-255)
//   - Coil 0 = enable (read/write)
//   - Coil 1 = blackout (write-only, triggers blackout on write 1)
type Server struct {
	cfg    *Config
	state  *dmx.State
	logger *slog.Logger
	mb     *mbserver.Server
	mu     sync.RWMutex
}

// NewServer creates a new Modbus TCP server
func NewServer(cfg *Config, state *dmx.State, logger *slog.Logger) *Server {
	return &Server{
		cfg:    cfg,
		state:  state,
		logger: logger,
	}
}

// Start starts the Modbus TCP server
func (s *Server) Start() error {
	s.mb = mbserver.NewServer()

	// Register custom handlers
	s.mb.RegisterFunctionHandler(3, s.handleReadHoldingRegisters)  // FC03
	s.mb.RegisterFunctionHandler(6, s.handleWriteSingleRegister)   // FC06
	s.mb.RegisterFunctionHandler(16, s.handleWriteMultipleRegisters) // FC16
	s.mb.RegisterFunctionHandler(1, s.handleReadCoils)             // FC01
	s.mb.RegisterFunctionHandler(5, s.handleWriteSingleCoil)       // FC05

	addr := s.cfg.Port
	if addr == "" {
		addr = ":502"
	}

	s.logger.Info("Modbus TCP server starting", "addr", addr)

	go func() {
		if err := s.mb.ListenTCP(addr); err != nil {
			s.logger.Error("Modbus TCP server error", "error", err)
		}
	}()

	return nil
}

// Stop stops the Modbus TCP server
func (s *Server) Stop() {
	if s.mb != nil {
		s.mb.Close()
		s.logger.Info("Modbus TCP server stopped")
	}
}

// FC03: Read Holding Registers (DMX channels)
func (s *Server) handleReadHoldingRegisters(_ *mbserver.Server, frame mbserver.Framer) ([]byte, *mbserver.Exception) {
	data := frame.GetData()
	if len(data) < 4 {
		return []byte{}, &mbserver.IllegalDataValue
	}

	startAddr := binary.BigEndian.Uint16(data[0:2])
	quantity := binary.BigEndian.Uint16(data[2:4])

	if startAddr+quantity > 512 {
		return []byte{}, &mbserver.IllegalDataAddress
	}

	channels := s.state.GetChannels()

	// Build response: each register = 1 channel (0-255 in low byte)
	resp := make([]byte, 1+quantity*2)
	resp[0] = byte(quantity * 2) // byte count

	for i := uint16(0); i < quantity; i++ {
		ch := startAddr + i
		val := uint16(channels[ch])
		binary.BigEndian.PutUint16(resp[1+i*2:], val)
	}

	return resp, &mbserver.Success
}

// FC06: Write Single Register (single DMX channel)
func (s *Server) handleWriteSingleRegister(_ *mbserver.Server, frame mbserver.Framer) ([]byte, *mbserver.Exception) {
	data := frame.GetData()
	if len(data) < 4 {
		return []byte{}, &mbserver.IllegalDataValue
	}

	addr := binary.BigEndian.Uint16(data[0:2])
	value := binary.BigEndian.Uint16(data[2:4])

	if addr >= 512 {
		return []byte{}, &mbserver.IllegalDataAddress
	}
	if value > 255 {
		value = 255
	}

	channel := int(addr) + 1 // DMX channels are 1-indexed
	if err := s.state.SetChannel(channel, uint8(value)); err != nil {
		s.logger.Warn("Modbus write failed", "ch", channel, "error", err)
		return []byte{}, &mbserver.SlaveDeviceFailure
	}

	s.logger.Debug("Modbus write", "ch", channel, "value", value)

	// Echo request as response
	return data[:4], &mbserver.Success
}

// FC16: Write Multiple Registers (multiple DMX channels)
func (s *Server) handleWriteMultipleRegisters(_ *mbserver.Server, frame mbserver.Framer) ([]byte, *mbserver.Exception) {
	data := frame.GetData()
	if len(data) < 5 {
		return []byte{}, &mbserver.IllegalDataValue
	}

	startAddr := binary.BigEndian.Uint16(data[0:2])
	quantity := binary.BigEndian.Uint16(data[2:4])
	byteCount := data[4]

	if startAddr+quantity > 512 {
		return []byte{}, &mbserver.IllegalDataAddress
	}
	if int(byteCount) != int(quantity)*2 || len(data) < 5+int(byteCount) {
		return []byte{}, &mbserver.IllegalDataValue
	}

	// Write each channel
	for i := uint16(0); i < quantity; i++ {
		value := binary.BigEndian.Uint16(data[5+i*2:])
		if value > 255 {
			value = 255
		}
		channel := int(startAddr+i) + 1
		if err := s.state.SetChannel(channel, uint8(value)); err != nil {
			s.logger.Warn("Modbus write failed", "ch", channel, "error", err)
		}
	}

	s.logger.Debug("Modbus write multiple", "start", startAddr+1, "count", quantity)

	// Response: start addr + quantity
	resp := make([]byte, 4)
	binary.BigEndian.PutUint16(resp[0:2], startAddr)
	binary.BigEndian.PutUint16(resp[2:4], quantity)
	return resp, &mbserver.Success
}

// FC01: Read Coils (enable status)
func (s *Server) handleReadCoils(_ *mbserver.Server, frame mbserver.Framer) ([]byte, *mbserver.Exception) {
	data := frame.GetData()
	if len(data) < 4 {
		return []byte{}, &mbserver.IllegalDataValue
	}

	startAddr := binary.BigEndian.Uint16(data[0:2])
	quantity := binary.BigEndian.Uint16(data[2:4])

	if startAddr+quantity > 2 {
		return []byte{}, &mbserver.IllegalDataAddress
	}

	// Coil 0 = enabled, Coil 1 = always 0 (blackout is write-only)
	var coils byte
	if s.state.IsEnabled() {
		coils |= 0x01
	}

	resp := []byte{1, coils} // byte count + coils byte
	return resp, &mbserver.Success
}

// FC05: Write Single Coil (enable/disable/blackout)
func (s *Server) handleWriteSingleCoil(_ *mbserver.Server, frame mbserver.Framer) ([]byte, *mbserver.Exception) {
	data := frame.GetData()
	if len(data) < 4 {
		return []byte{}, &mbserver.IllegalDataValue
	}

	addr := binary.BigEndian.Uint16(data[0:2])
	value := binary.BigEndian.Uint16(data[2:4])

	on := value == 0xFF00

	switch addr {
	case 0: // Enable/disable
		if on {
			if err := s.state.Enable(); err != nil {
				return []byte{}, &mbserver.SlaveDeviceFailure
			}
			s.logger.Info("Modbus: DMX enabled")
		} else {
			if err := s.state.Disable(); err != nil {
				return []byte{}, &mbserver.SlaveDeviceFailure
			}
			s.logger.Info("Modbus: DMX disabled")
		}
	case 1: // Blackout (only on write 1)
		if on {
			if err := s.state.Blackout(); err != nil {
				return []byte{}, &mbserver.SlaveDeviceFailure
			}
			s.logger.Info("Modbus: Blackout triggered")
		}
	default:
		return []byte{}, &mbserver.IllegalDataAddress
	}

	// Echo request as response
	return data[:4], &mbserver.Success
}
