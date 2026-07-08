import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Dev proxy avoids CORS entirely and lets the frontend talk to the API
// via relative paths (/api, /ws) regardless of host/port.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/api": {
        target: "http://127.0.0.1:8080",
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api/, ""),
      },
      "/ws": {
        target: "ws://127.0.0.1:8080",
        ws: true,
      },
    },
  },
});
