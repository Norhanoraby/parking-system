const mongoose = require("mongoose");

const logSchema = new mongoose.Schema({
  eventType: String,
  cardId: String,
  fee: Number,
  slots: Number,
  userName: String,
  role: String,
}, { timestamps: true });

module.exports = mongoose.model("Log", logSchema);