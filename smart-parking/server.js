const dotenv = require("dotenv");
dotenv.config();

const express = require("express");
const mongoose = require("mongoose");
const cors = require("cors");

console.log("MONGO_URI exists:", !!process.env.MONGO_URI);

const app = express();

app.use(cors());
app.use(express.json());
// THIS IS THE MAGIC LINE THAT SHOWS YOUR HTML FILES:
app.use(express.static("public")); 

mongoose
  .connect(process.env.MONGO_URI)
  .then(() => console.log("MongoDB connected"))
  .catch((err) => console.log("MongoDB connection error:", err));

const userSchema = new mongoose.Schema(
  {
    name: String,
    cardId: { type: String, required: true, unique: true },
    role: { type: String, default: "user" },
  },
  { timestamps: true }
);

const logSchema = new mongoose.Schema(
  {
    eventType: String,
    cardId: String,
    fee: Number,
    slots: Number,
    userName: String,
    role: String,
  },
  { timestamps: true }
);

const User = mongoose.model("User", userSchema);
const Log = mongoose.model("Log", logSchema);

app.post("/api/users", async (req, res) => {
  try {
    const user = await User.create(req.body);
    res.json(user);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.post("/api/rfid-event", async (req, res) => {
  try {
    const { eventType, cardId, fee, slots } = req.body;

    const user = await User.findOne({ cardId });

    const log = await Log.create({
      eventType,
      cardId,
      fee,
      slots,
      userName: user ? user.name : "Unknown",
      role: user ? user.role : "unknown",
    });

    console.log("New RFID event:", log);

    res.json({
      success: true,
      log,
      user,
      openAdmin: user?.role === "admin",
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.get("/api/logs", async (req, res) => {
  try {
    const logs = await Log.find().sort({ createdAt: -1 });
    res.json(logs);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.listen(3000, () => {
  console.log("Server running on port 3000");
});