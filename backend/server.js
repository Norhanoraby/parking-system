require('dotenv').config();
const express = require('express');
const mongoose = require('mongoose');
const cors = require('cors');
const path = require('path');

const app = express();

// Dynamic CORS Reflection Middleware to allow uninterrupted streaming across layout interfaces
app.use(cors({
  origin: function (origin, callback) {
    if (!origin) return callback(null, true);
    return callback(null, true);
  },
  credentials: true,
  methods: ['GET', 'POST']
}));
app.use(express.json());

let clients = [];

app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, 'dashboard.html'));
});

// ---- FIXED SSE STREAM ROUTE ----
app.get('/api/stream', (req, res) => {
  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection', 'keep-alive');
  
  // Force headers to flush immediately so Chrome changes status from "Pending" to "Active"
  if (res.flushHeaders) {
    res.flushHeaders();
  } else if (res.flush) {
    res.flush();
  }

  // Send an immediate SSE comment line to break the browser response buffer
  res.write(': open\n\n');

  clients.push(res);
  
  req.on('close', () => { 
    clients = clients.filter(c => c !== res); 
  });
});

mongoose.connect(process.env.MONGODB_URI)
  .then(() => console.log('✅ MongoDB connected'))
  .catch(err => console.error('❌ MongoDB error:', err));

const userSchema = new mongoose.Schema({
  cardId: { type: String, unique: true, required: true, lowercase: true },
  name: String,
  phone: String,
  role: { type: String, default: 'user' }
}, { timestamps: true });

const logSchema = new mongoose.Schema({
  event: String,
  cardId: { type: String, lowercase: true },
  userName: String,
  fee: Number,
  penalty: Number,
  duration: Number,
  slotsLeft: Number,
  slotStates: Object,
  timestamp: { type: Date, default: Date.now }
});

const User = mongoose.model('User', userSchema, 'users');
const Log  = mongoose.model('Log',  logSchema,  'logs');

app.post('/api/event', async (req, res) => {
  try {
    const { event, uid, fee, penalty, duration, slotsLeft, slotStates } = req.body;
    const cardId = (uid || '').toLowerCase();

    let userName = 'Unknown';
    if (cardId && cardId !== 'none') {
      const user = await User.findOne({ cardId });
      if (user) userName = user.name;
    }

    const log = new Log({ event, cardId, userName, fee, penalty, duration, slotsLeft, slotStates });
    await log.save();

    const payload = { event, cardId, userName, fee, penalty, duration, slotsLeft, slotStates };
    
    // Broadcast updates safely to all active dashboard interfaces
    clients.forEach(client => client.write(`data: ${JSON.stringify(payload)}\n\n`));

    res.json({ success: true, userName });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.get('/api/verify/:uid', async (req, res) => {
  const cardId = req.params.uid.toLowerCase();
  const user = await User.findOne({ cardId });
  res.json({ found: !!user, user });
});

app.get('/api/logs', async (req, res) => {
  const logs = await Log.find().sort({ timestamp: -1 }).limit(100);
  res.json(logs);
});

app.get('/api/user/:uid/fee', async (req, res) => {
  const cardId = req.params.uid.toLowerCase();
  const user = await User.findOne({ cardId });
  const lastLog = await Log.findOne({
    cardId,
    event: { $in: ['EXIT_PENDING_PAYMENT', 'PAYMENT_DONE', 'ENTRY'] }
  }).sort({ timestamp: -1 });
  res.json({ user, lastLog });
});

app.post('/api/users', async (req, res) => {
  try {
    const data = { ...req.body };
    if (data.uid) { data.cardId = data.uid.toLowerCase(); delete data.uid; }
    if (data.cardId) data.cardId = data.cardId.toLowerCase();
    const user = new User(data);
    await user.save();
    res.json(user);
  } catch (err) {
    res.status(400).json({ error: err.message });
  }
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, '0.0.0.0', () => {
  console.log(`🚀 Server running on port ${PORT}`);
});
