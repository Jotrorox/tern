CDC report: 8 crossings
  Level > s: sync2, sys -> pix, signal flag (cdc_ok.tern:16)
  Bridge > f: async_fifo, a -> b, signal push.valid (cdc_ok.tern:35)
  Bridge > p: pulse, a -> b, signal pulse (cdc_ok.tern:39)
  Bridge > h: handshake, a -> b, signal h.in.valid (cdc_ok.tern:43)
  Resets > rs: reset_sync, async -> sys, signal ext_rst (cdc_ok.tern:54)
  Resets > rp: reset_sync, async -> pix, signal ext_rst (cdc_ok.tern:56)
  Bridged > rb: reset_bridge, sys -> pix, signal sys_rst (cdc_ok.tern:72)
  Config: unsafe, sys -> pix, signal m (cdc_ok.tern:90)
