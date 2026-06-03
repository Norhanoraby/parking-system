const mongoose = require("mongoose");

const userSchema = new mongoose.Schema({
  name: String,
  cardId: {
    type: String,
    required: true,
    unique: true,
  },
  role: {
    type: String,
    default: "user", // user or admin
  },
  balance: {
    type: Number,
    default: 0,
  },
}, { timestamps: true });

module.exports = mongoose.model("User", userSchema);