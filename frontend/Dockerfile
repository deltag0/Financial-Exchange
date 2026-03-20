# Multi-stage build template for a typical frontend (React, Vue, etc.)

# --- Build Stage ---
FROM node:20-alpine AS build
WORKDIR /app

# For now we have no frontend code, but once you do:
# COPY package*.json ./
# RUN npm install
# COPY . .
# RUN npm run build

# --- Production Stage ---
FROM nginx:alpine

# Copy the build output to replace the default nginx contents
# COPY --from=build /app/build /usr/share/nginx/html
# Or for Vite/Vue:
# COPY --from=build /app/dist /usr/share/nginx/html

# Expose port 80
EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
