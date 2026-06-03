require('dotenv').config();
const mongoose = require('mongoose');

// --- Same schema as server.js ---
const userSchema = new mongoose.Schema({
  cardId: { type: String, unique: true, required: true, lowercase: true },
  name: String,
  phone: String,
  role: { type: String, default: 'user' }
}, { timestamps: true });

const User = mongoose.model('User', userSchema, 'users');

// --- The users to add ---
// These match the card UIDs from your STM32 code
const usersToSeed = [
  {
    cardId: 'd39cd72a',     // admin in STM32 code
    name: 'farida',
    phone: '01001234567',
    role: 'admin'
  },
  {
    cardId: '031093aa',     // card1 in STM32 code
    name: 'zeina',
    phone: '01007654321',
    role: 'user'
  },
  {
    cardId: 'd301d32a',     // card2 in STM32 code
    name: 'caroll',
    phone: '01009998888',
    role: 'user'
  },
  {
    cardId: '638ecc29',     // card3 in STM32 code
    name: 'norhan',
    phone: '01005556666',
    role: 'user'
  },
   {
    cardId: '4af59497',     // card4 in STM32 code
    name: 'shapinam',
    phone: '01005556666',
    role: 'user'
  },
  {
  cardId: '9b01ec05',
  name: 'manual gate card',
  phone: '00000000000',
  role: 'manual'
},
{
  cardId: '6052f261',
  name: 'vip card',
  phone: '00000000000',
  role: 'user'
}
];

async function seed() {
  try {
    await mongoose.connect(process.env.MONGODB_URI);
    console.log('✅ Connected to MongoDB');

    // Clear out any existing users first (clean slate)
    const deleted = await User.deleteMany({});
    console.log(`🗑️  Cleared ${deleted.deletedCount} existing users`);

    // Insert all users
    const inserted = await User.insertMany(usersToSeed);
    console.log(`✅ Inserted ${inserted.length} users:\n`);

    inserted.forEach(u => {
      console.log(`   ${u.role === 'admin' ? '👑' : '👤'}  ${u.name.padEnd(10)} | ${u.cardId} | ${u.role}`);
    });

    console.log('\n🎉 Database seeded successfully!');
  } catch (err) {
    console.error('❌ Seed failed:', err.message);
  } finally {
    await mongoose.disconnect();
    process.exit(0);
  }
}

seed();