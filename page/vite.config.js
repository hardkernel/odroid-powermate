// SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
// SPDX-License-Identifier: GPL-3.0-only

import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import viteCompression from 'vite-plugin-compression';

export default defineConfig({
  plugins: [
    viteSingleFile(),
    viteCompression(),
  ],
});
