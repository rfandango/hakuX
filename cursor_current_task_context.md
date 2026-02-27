# Current task context
_Exported on 2/26/2026 at 19:15:22 GMT from Cursor (2.3.34)_

---

**User**

pleasse continue

---

**Cursor**

This is a critical finding. `pgraph_vk_begin_nondraw_commands` returns the **main command buffer**, not the aux command buffer. The "nondraw" commands (texture uploads) go into the main CB outside the render pass. Let me now understand the render pass restart behavior.

Now I see the crucial difference. With `OPT_TEX_NONDRAW_CMD=1`, texture uploads go into the **main** command buffer and require ending the render pass. With `=0`, they use single-time commands in the AUX CB without touching the render pass. Let me trace the call flow to understand when render pass breaks happen.

Now I need to understand when `bind_textures` is called relative to the render pass. Let me check.

