const http = require('http');

const port = process.env.PORT || 3000;

const server = http.createServer((req, res) => {
  res.statusCode = 200;
  res.setHeader('Content-Type', 'text/plain');
  res.end('Hello from the backend server!\n');
});

server.listen(port, () => {
  console.log(`Backend server running on port ${port}`);
});
