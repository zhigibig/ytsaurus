unsigned char malloc_udf_bc[] = {
  0x42, 0x43, 0xc0, 0xde, 0x21, 0x0c, 0x00, 0x00, 0x59, 0x01, 0x00, 0x00,
  0x0b, 0x82, 0x20, 0x00, 0x02, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
  0x07, 0x81, 0x23, 0x91, 0x41, 0xc8, 0x04, 0x49, 0x06, 0x10, 0x32, 0x39,
  0x92, 0x01, 0x84, 0x0c, 0x25, 0x05, 0x08, 0x19, 0x1e, 0x04, 0x8b, 0x62,
  0x80, 0x10, 0x45, 0x02, 0x42, 0x92, 0x0b, 0x42, 0x84, 0x10, 0x32, 0x14,
  0x38, 0x08, 0x18, 0x49, 0x0a, 0x32, 0x44, 0x24, 0x48, 0x0a, 0x90, 0x21,
  0x23, 0xc4, 0x52, 0x80, 0x0c, 0x19, 0x21, 0x72, 0x24, 0x07, 0xc8, 0x08,
  0x11, 0x62, 0xa8, 0xa0, 0xa8, 0x40, 0xc6, 0xf0, 0x01, 0x00, 0x00, 0x00,
  0x51, 0x18, 0x00, 0x00, 0x9c, 0x00, 0x00, 0x00, 0x1b, 0x6a, 0x23, 0xf8,
  0xff, 0xff, 0xff, 0xff, 0x01, 0x90, 0x80, 0x30, 0x20, 0xd8, 0xa1, 0x1c,
  0xe6, 0x61, 0x1e, 0xda, 0x00, 0x1e, 0xe4, 0xa1, 0x1c, 0xc6, 0x21, 0x1d,
  0xe6, 0xa1, 0x1c, 0xda, 0xc0, 0x1c, 0xe0, 0xa1, 0x1d, 0xc2, 0x81, 0x1c,
  0x00, 0x73, 0x08, 0x07, 0x76, 0x98, 0x87, 0x72, 0x00, 0x08, 0x77, 0x78,
  0x87, 0x36, 0x30, 0x07, 0x79, 0x08, 0x87, 0x76, 0x28, 0x87, 0x36, 0x80,
  0x87, 0x77, 0x48, 0x07, 0x77, 0xa0, 0x87, 0x72, 0x90, 0x87, 0x36, 0x28,
  0x07, 0x76, 0x48, 0x87, 0x76, 0x00, 0xe8, 0x41, 0x1e, 0xea, 0xa1, 0x1c,
  0x80, 0xc1, 0x1d, 0xde, 0xa1, 0x0d, 0xcc, 0x41, 0x1e, 0xc2, 0xa1, 0x1d,
  0xca, 0xa1, 0x0d, 0xe0, 0xe1, 0x1d, 0xd2, 0xc1, 0x1d, 0xe8, 0xa1, 0x1c,
  0xe4, 0xa1, 0x0d, 0xca, 0x81, 0x1d, 0xd2, 0xa1, 0x1d, 0xda, 0xc0, 0x1d,
  0xde, 0xc1, 0x1d, 0xda, 0x80, 0x1d, 0xca, 0x21, 0x1c, 0xcc, 0x01, 0x20,
  0xdc, 0xe1, 0x1d, 0xda, 0x20, 0x1d, 0xdc, 0xc1, 0x1c, 0xe6, 0xa1, 0x0d,
  0xcc, 0x01, 0x1e, 0xda, 0xa0, 0x1d, 0xc2, 0x81, 0x1e, 0xd0, 0x01, 0x30,
  0x87, 0x70, 0x60, 0x87, 0x79, 0x28, 0x07, 0x80, 0x70, 0x87, 0x77, 0x68,
  0x03, 0x77, 0x08, 0x07, 0x77, 0x98, 0x87, 0x36, 0x30, 0x07, 0x78, 0x68,
  0x83, 0x76, 0x08, 0x07, 0x7a, 0x40, 0x07, 0xc0, 0x1c, 0xc2, 0x81, 0x1d,
  0xe6, 0xa1, 0x1c, 0x00, 0x62, 0x1e, 0xe8, 0x21, 0x1c, 0xc6, 0x61, 0x1d,
  0xda, 0x00, 0x1e, 0xe4, 0xe1, 0x1d, 0xe8, 0xa1, 0x1c, 0xc6, 0x81, 0x1e,
  0xde, 0x41, 0x1e, 0xda, 0x40, 0x1c, 0xea, 0xc1, 0x1c, 0xcc, 0xa1, 0x1c,
  0xe4, 0xa1, 0x0d, 0xe6, 0x21, 0x1d, 0xf4, 0xa1, 0x1c, 0x00, 0x3c, 0x00,
  0x88, 0x7a, 0x70, 0x87, 0x79, 0x08, 0x07, 0x73, 0x28, 0x87, 0x36, 0x30,
  0x07, 0x78, 0x68, 0x83, 0x76, 0x08, 0x07, 0x7a, 0x40, 0x07, 0xc0, 0x1c,
  0xc2, 0x81, 0x1d, 0xe6, 0xa1, 0x1c, 0x00, 0xa2, 0x1e, 0xe6, 0xa1, 0x1c,
  0xda, 0x60, 0x1e, 0xde, 0xc1, 0x1c, 0xe8, 0xa1, 0x0d, 0xcc, 0x81, 0x1d,
  0xde, 0x21, 0x1c, 0xe8, 0x01, 0x30, 0x87, 0x70, 0x60, 0x87, 0x79, 0x28,
  0x07, 0x60, 0x03, 0x21, 0x00, 0x40, 0xb2, 0x61, 0x36, 0x86, 0xff, 0xff,
  0xff, 0xff, 0x1f, 0x00, 0x89, 0x60, 0x87, 0x72, 0x98, 0x87, 0x79, 0x68,
  0x03, 0x78, 0x90, 0x87, 0x72, 0x18, 0x87, 0x74, 0x98, 0x87, 0x72, 0x68,
  0x03, 0x73, 0x80, 0x87, 0x76, 0x08, 0x07, 0x72, 0x00, 0xcc, 0x21, 0x1c,
  0xd8, 0x61, 0x1e, 0xca, 0x01, 0x20, 0xdc, 0xe1, 0x1d, 0xda, 0xc0, 0x1c,
  0xe4, 0x21, 0x1c, 0xda, 0xa1, 0x1c, 0xda, 0x00, 0x1e, 0xde, 0x21, 0x1d,
  0xdc, 0x81, 0x1e, 0xca, 0x41, 0x1e, 0xda, 0xa0, 0x1c, 0xd8, 0x21, 0x1d,
  0xda, 0x01, 0xa0, 0x07, 0x79, 0xa8, 0x87, 0x72, 0x00, 0x06, 0x77, 0x78,
  0x87, 0x36, 0x30, 0x07, 0x79, 0x08, 0x87, 0x76, 0x28, 0x87, 0x36, 0x80,
  0x87, 0x77, 0x48, 0x07, 0x77, 0xa0, 0x87, 0x72, 0x90, 0x87, 0x36, 0x28,
  0x07, 0x76, 0x48, 0x87, 0x76, 0x68, 0x03, 0x77, 0x78, 0x07, 0x77, 0x68,
  0x03, 0x76, 0x28, 0x87, 0x70, 0x30, 0x07, 0x80, 0x70, 0x87, 0x77, 0x68,
  0x83, 0x74, 0x70, 0x07, 0x73, 0x98, 0x87, 0x36, 0x30, 0x07, 0x78, 0x68,
  0x83, 0x76, 0x08, 0x07, 0x7a, 0x40, 0x07, 0xc0, 0x1c, 0xc2, 0x81, 0x1d,
  0xe6, 0xa1, 0x1c, 0x00, 0xc2, 0x1d, 0xde, 0xa1, 0x0d, 0xdc, 0x21, 0x1c,
  0xdc, 0x61, 0x1e, 0xda, 0xc0, 0x1c, 0xe0, 0xa1, 0x0d, 0xda, 0x21, 0x1c,
  0xe8, 0x01, 0x1d, 0x00, 0x73, 0x08, 0x07, 0x76, 0x98, 0x87, 0x72, 0x00,
  0x88, 0x79, 0xa0, 0x87, 0x70, 0x18, 0x87, 0x75, 0x68, 0x03, 0x78, 0x90,
  0x87, 0x77, 0xa0, 0x87, 0x72, 0x18, 0x07, 0x7a, 0x78, 0x07, 0x79, 0x68,
  0x03, 0x71, 0xa8, 0x07, 0x73, 0x30, 0x87, 0x72, 0x90, 0x87, 0x36, 0x98,
  0x87, 0x74, 0xd0, 0x87, 0x72, 0x00, 0xf0, 0x00, 0x20, 0xea, 0xc1, 0x1d,
  0xe6, 0x21, 0x1c, 0xcc, 0xa1, 0x1c, 0xda, 0xc0, 0x1c, 0xe0, 0xa1, 0x0d,
  0xda, 0x21, 0x1c, 0xe8, 0x01, 0x1d, 0x00, 0x73, 0x08, 0x07, 0x76, 0x98,
  0x87, 0x72, 0x00, 0x88, 0x7a, 0x98, 0x87, 0x72, 0x68, 0x83, 0x79, 0x78,
  0x07, 0x73, 0xa0, 0x87, 0x36, 0x30, 0x07, 0x76, 0x78, 0x87, 0x70, 0xa0,
  0x07, 0xc0, 0x1c, 0xc2, 0x81, 0x1d, 0xe6, 0xa1, 0x1c, 0x80, 0x0d, 0x04,
  0xf1, 0xff, 0xff, 0xff, 0xff, 0x03, 0x20, 0x01, 0x49, 0x18, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x13, 0x82, 0x60, 0x82, 0x20, 0x0c, 0x13, 0x04,
  0x81, 0x00, 0x00, 0x00, 0x89, 0x20, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
  0x32, 0x22, 0x08, 0x09, 0x20, 0x64, 0x85, 0x04, 0x13, 0x22, 0xa4, 0x84,
  0x04, 0x13, 0x22, 0xe3, 0x84, 0xa1, 0x90, 0x14, 0x12, 0x4c, 0x88, 0x8c,
  0x0b, 0x84, 0x84, 0x4c, 0x10, 0x38, 0x73, 0x04, 0xa0, 0x70, 0x98, 0x34,
  0x45, 0x94, 0x30, 0xf9, 0xad, 0x77, 0x11, 0x02, 0x35, 0x21, 0x4e, 0xc3,
  0x39, 0xcd, 0x44, 0x5c, 0xd3, 0x18, 0x01, 0x40, 0x51, 0x06, 0x10, 0xa0,
  0x99, 0x23, 0x40, 0xa8, 0x8a, 0x60, 0x40, 0x37, 0x10, 0x30, 0x47, 0x00,
  0x06, 0x24, 0x04, 0x23, 0x00, 0x00, 0x00, 0x00, 0x13, 0x26, 0x7c, 0xc0,
  0x03, 0x3b, 0xf8, 0x05, 0x3b, 0xa0, 0x83, 0x36, 0x80, 0x87, 0x71, 0x68,
  0x03, 0x76, 0x48, 0x07, 0x77, 0xa8, 0x07, 0x7c, 0x68, 0x83, 0x73, 0x70,
  0x87, 0x7a, 0xd8, 0x50, 0x06, 0xe5, 0xd0, 0x06, 0xed, 0xa0, 0x07, 0xe5,
  0xd0, 0x06, 0xe9, 0x60, 0x07, 0x74, 0xa0, 0x07, 0x76, 0x40, 0x07, 0x6d,
  0x60, 0x0e, 0x78, 0x00, 0x07, 0x7a, 0x10, 0x07, 0x72, 0x80, 0x07, 0x6d,
  0xe0, 0x0e, 0x78, 0xa0, 0x07, 0x71, 0x60, 0x07, 0x7a, 0x30, 0x07, 0x72,
  0xa0, 0x07, 0x76, 0x40, 0x07, 0x6d, 0x30, 0x0b, 0x71, 0x20, 0x07, 0x78,
  0x30, 0xc4, 0x21, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x80, 0x21, 0x0e, 0x02, 0x04, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xe4, 0x61, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x1a, 0x03, 0x4c, 0x90,
  0x46, 0x02, 0x13, 0xc4, 0x5b, 0xa8, 0x12, 0xab, 0x73, 0xa3, 0xab, 0x03,
  0x19, 0x63, 0x0b, 0x73, 0x3b, 0x03, 0xb1, 0x2b, 0x93, 0x9b, 0x4b, 0x7b,
  0x73, 0x03, 0x99, 0x71, 0xb1, 0x71, 0x81, 0x69, 0x99, 0xb3, 0x73, 0x93,
  0x91, 0xc9, 0xc9, 0xa1, 0xb1, 0x69, 0x89, 0xf1, 0x2b, 0xc3, 0x83, 0x8b,
  0x01, 0x41, 0x11, 0x93, 0x0b, 0x73, 0x1b, 0x43, 0x2b, 0x9b, 0x7b, 0x91,
  0x2b, 0x63, 0x2b, 0x0b, 0x9b, 0x2b, 0xfb, 0x9a, 0xb1, 0x49, 0x01, 0x41,
  0x11, 0x0b, 0x9b, 0x2b, 0x23, 0x03, 0x79, 0x73, 0x03, 0x61, 0x62, 0xb2,
  0x6a, 0x02, 0x99, 0x71, 0xb1, 0x71, 0x81, 0x49, 0xd9, 0x10, 0x04, 0x55,
  0xd8, 0xd8, 0xec, 0xda, 0x5c, 0xd2, 0xc8, 0xca, 0xdc, 0xe8, 0xa6, 0x04,
  0x01, 0x00, 0x00, 0x00, 0x79, 0x18, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x33, 0x08, 0x80, 0x1c, 0xc4, 0xe1, 0x1c, 0x66, 0x14, 0x01, 0x3d, 0x88,
  0x43, 0x38, 0x84, 0xc3, 0x8c, 0x42, 0x80, 0x07, 0x79, 0x78, 0x07, 0x73,
  0x98, 0x71, 0x0c, 0xe6, 0x00, 0x0f, 0xed, 0x10, 0x0e, 0xf4, 0x80, 0x0e,
  0x33, 0x0c, 0x42, 0x1e, 0xc2, 0xc1, 0x1d, 0xce, 0xa1, 0x1c, 0x66, 0x30,
  0x05, 0x3d, 0x88, 0x43, 0x38, 0x84, 0x83, 0x1b, 0xcc, 0x03, 0x3d, 0xc8,
  0x43, 0x3d, 0x8c, 0x03, 0x3d, 0xcc, 0x78, 0x8c, 0x74, 0x70, 0x07, 0x7b,
  0x08, 0x07, 0x79, 0x48, 0x87, 0x70, 0x70, 0x07, 0x7a, 0x70, 0x03, 0x76,
  0x78, 0x87, 0x70, 0x20, 0x87, 0x19, 0xcc, 0x11, 0x0e, 0xec, 0x90, 0x0e,
  0xe1, 0x30, 0x0f, 0x6e, 0x30, 0x0f, 0xe3, 0xf0, 0x0e, 0xf0, 0x50, 0x0e,
  0x33, 0x10, 0xc4, 0x1d, 0xde, 0x21, 0x1c, 0xd8, 0x21, 0x1d, 0xc2, 0x61,
  0x1e, 0x66, 0x30, 0x89, 0x3b, 0xbc, 0x83, 0x3b, 0xd0, 0x43, 0x39, 0xb4,
  0x03, 0x3c, 0xbc, 0x83, 0x3c, 0x84, 0x03, 0x3b, 0xcc, 0xf0, 0x14, 0x76,
  0x60, 0x07, 0x7b, 0x68, 0x07, 0x37, 0x68, 0x87, 0x72, 0x68, 0x07, 0x37,
  0x80, 0x87, 0x70, 0x90, 0x87, 0x70, 0x60, 0x07, 0x76, 0x28, 0x07, 0x76,
  0xf8, 0x05, 0x76, 0x78, 0x87, 0x77, 0x80, 0x87, 0x5f, 0x08, 0x87, 0x71,
  0x18, 0x87, 0x72, 0x98, 0x87, 0x79, 0x98, 0x81, 0x2c, 0xee, 0xf0, 0x0e,
  0xee, 0xe0, 0x0e, 0xf5, 0xc0, 0x0e, 0xec, 0x00, 0x71, 0x20, 0x00, 0x00,
  0x05, 0x00, 0x00, 0x00, 0x06, 0xa0, 0x30, 0xc0, 0xb2, 0x38, 0xc2, 0x4f,
  0x0d, 0x85, 0x05, 0x18, 0x0c, 0xb0, 0x2c, 0x8e, 0x00, 0x00, 0x00, 0x00,
  0x61, 0x20, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x13, 0x04, 0x41, 0x2c,
  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x25, 0x00, 0x00,
  0x33, 0x11, 0x8b, 0x42, 0x10, 0x33, 0x11, 0x8c, 0x42, 0x10, 0x83, 0x11,
  0x42, 0x41, 0x00, 0x83, 0x11, 0x01, 0x41, 0x00, 0x14, 0x90, 0x11, 0x03,
  0x62, 0x00, 0x8e, 0xe0, 0x02, 0xc8, 0x0c, 0xc2, 0x81, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x26, 0x70, 0x08, 0x4e, 0x33, 0x11, 0xd7, 0x64,
  0x03, 0x41, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
unsigned int malloc_udf_bc_len = 1392;
