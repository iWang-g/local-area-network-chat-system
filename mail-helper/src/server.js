'use strict';

require('dotenv').config();

const express = require('express');
const nodemailer = require('nodemailer');

const PORT = Number(process.env.PORT) || 8787;
const SECRET = (process.env.MAIL_HELPER_SECRET || '').trim();
const SMTP_URL = (process.env.SMTP_URL || '').trim();
const MAIL_FROM = process.env.MAIL_FROM || '"LANCS" <noreply@localhost>';
const MAIL_SUBJECT = process.env.MAIL_SUBJECT || '您的注册验证码';

const app = express();
app.use(express.json({ limit: '32kb' }));

function authMiddleware(req, res, next) {
  if (!SECRET) {
    return next();
  }
  const h = req.headers.authorization || '';
  const token = h.startsWith('Bearer ') ? h.slice(7).trim() : '';
  if (token !== SECRET) {
    return res.status(401).json({ ok: false, error: 'unauthorized' });
  }
  return next();
}

app.get('/health', (req, res) => {
  res.json({ ok: true, service: 'lancs-mail-helper' });
});

app.post('/v1/send-register-code', authMiddleware, async (req, res) => {
  const body = req.body || {};
  const email = typeof body.email === 'string' ? body.email.trim() : '';
  const code = typeof body.code === 'string' ? body.code.trim() : '';
  const purpose = typeof body.purpose === 'string' ? body.purpose.trim() : 'register';

  if (!email || !code) {
    return res.status(400).json({ ok: false, error: 'missing email or code' });
  }
  if (purpose !== 'register') {
    return res.status(400).json({ ok: false, error: 'unsupported purpose' });
  }

  console.log(`[mail-helper] recv POST /v1/send-register-code to=${email}`);

  if (!SMTP_URL) {
    console.log(`[mail-helper] dry-run register code to=${email} code=${code}`);
    return res.json({ ok: true, dryRun: true });
  }

  try {
    const transporter = nodemailer.createTransport(SMTP_URL);
    await transporter.sendMail({
      from: MAIL_FROM,
      to: email,
      subject: MAIL_SUBJECT,
      text: `您的注册验证码为：${code}，5 分钟内有效。如非本人操作请忽略。`,
      html: `<p>您的注册验证码为：<strong>${escapeHtml(code)}</strong>，5 分钟内有效。</p><p>如非本人操作请忽略。</p>`,
    });
    console.log(`[mail-helper] sent register code to=${email}`);
    return res.json({ ok: true });
  } catch (e) {
    console.error('[mail-helper] send failed', e);
    return res.status(500).json({ ok: false, error: e.message || String(e) });
  }
});

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

app.listen(PORT, () => {
  console.log(`[mail-helper] listening on http://0.0.0.0:${PORT}`);
  console.log(
    `[mail-helper] route: POST http://127.0.0.1:${PORT}/v1/send-register-code | SMTP_URL=${SMTP_URL ? 'set' : 'empty (dry-run)'}`
  );
});
