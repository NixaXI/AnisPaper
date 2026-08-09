import type { Config } from "tailwindcss";

export default {
  content: ["./index.html", "./src/**/*.{ts,tsx}"],
  theme: {
    extend: {
      colors: {
        ocean: "#0A0D14",
        panel: "#101622",
        soda: "#FFD000",
        star: "#00C2FF",
        idol: "#FF6B8B"
      }
    }
  },
  plugins: []
} satisfies Config;
