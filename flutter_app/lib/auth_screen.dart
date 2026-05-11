// lib/auth_screen.dart
// Login (phone + OTP) and device pairing screens.
// Firebase path written on claim: user_sites/{uid} = ['site01']
// Pairing codes live at:          pairing_codes/{CODE} = {site_id, claimed}

import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_database/firebase_database.dart';

// ─── AuthGate ─────────────────────────────────────────────────────────────────
// Sits at the root of the widget tree.
// • Not logged in        → LoginScreen
// • Logged in, no sites  → DeviceCodeScreen
// • Logged in, has sites → dashboardBuilder(siteIds)
class AuthGate extends StatelessWidget {
  final Widget Function(List<String> siteIds) dashboardBuilder;
  const AuthGate({super.key, required this.dashboardBuilder});

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<User?>(
      stream: FirebaseAuth.instance.authStateChanges(),
      builder: (context, snap) {
        if (snap.connectionState == ConnectionState.waiting) {
          return const _Splash();
        }
        final user = snap.data;
        if (user == null) return const LoginScreen();
        return _SiteGate(uid: user.uid, dashboardBuilder: dashboardBuilder);
      },
    );
  }
}

// ─── _SiteGate ────────────────────────────────────────────────────────────────
// Reads user_sites/{uid} from Firebase RTDB.
// Routes to DeviceCodeScreen if empty, dashboard if sites are assigned.
class _SiteGate extends StatelessWidget {
  final String uid;
  final Widget Function(List<String> siteIds) dashboardBuilder;
  const _SiteGate({required this.uid, required this.dashboardBuilder});

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<DatabaseEvent>(
      stream: FirebaseDatabase.instance.ref('user_sites/$uid').onValue,
      builder: (context, snap) {
        if (snap.connectionState == ConnectionState.waiting) {
          return const _Splash();
        }
        final val = snap.data?.snapshot.value;
        if (val == null) return DeviceCodeScreen(uid: uid);

        List<String> siteIds = [];
        if (val is List) {
          siteIds = val.whereType<String>().toList();
        } else if (val is Map) {
          siteIds = val.values.whereType<String>().toList();
        }
        if (siteIds.isEmpty) return DeviceCodeScreen(uid: uid);
        return dashboardBuilder(siteIds);
      },
    );
  }
}

// ─── Splash ───────────────────────────────────────────────────────────────────
class _Splash extends StatelessWidget {
  const _Splash();
  @override
  Widget build(BuildContext context) {
    return const Scaffold(
      body: Center(child: CircularProgressIndicator()),
    );
  }
}

// ─── LoginScreen ──────────────────────────────────────────────────────────────
// Step 1: Enter 10-digit mobile number (+91 prefix hardcoded).
// Step 2: Enter 6-digit OTP sent by Firebase.
// On success: FirebaseAuth state changes → AuthGate re-routes automatically.
class LoginScreen extends StatefulWidget {
  const LoginScreen({super.key});
  @override
  State<LoginScreen> createState() => _LoginScreenState();
}

class _LoginScreenState extends State<LoginScreen> {
  final _auth      = FirebaseAuth.instance;
  final _phoneCtrl = TextEditingController();
  final _otpCtrl   = TextEditingController();

  bool   _otpSent        = false;
  bool   _loading        = false;
  String _verificationId = '';
  int?   _resendToken;
  int    _resendSeconds  = 0;
  Timer? _resendTimer;
  String _error          = '';

  @override
  void dispose() {
    _phoneCtrl.dispose();
    _otpCtrl.dispose();
    _resendTimer?.cancel();
    super.dispose();
  }

  // ── Send / resend OTP ────────────────────────────────────────────────────────
  Future<void> _sendOtp({bool resend = false}) async {
    final raw = _phoneCtrl.text.trim();
    if (raw.length != 10) {
      setState(() => _error = 'Enter a valid 10-digit mobile number');
      return;
    }
    setState(() { _loading = true; _error = ''; });

    await _auth.verifyPhoneNumber(
      phoneNumber: '+91$raw',
      forceResendingToken: resend ? _resendToken : null,
      timeout: const Duration(seconds: 60),
      verificationCompleted: (PhoneAuthCredential cred) => _signIn(cred),
      verificationFailed: (FirebaseAuthException e) {
        if (!mounted) return;
        setState(() { _loading = false; _error = e.message ?? 'Verification failed'; });
      },
      codeSent: (String vid, int? token) {
        if (!mounted) return;
        setState(() {
          _loading        = false;
          _otpSent        = true;
          _verificationId = vid;
          _resendToken    = token;
          _error          = '';
        });
        _startCountdown();
      },
      codeAutoRetrievalTimeout: (String vid) { _verificationId = vid; },
    );
  }

  void _startCountdown() {
    setState(() => _resendSeconds = 30);
    _resendTimer?.cancel();
    _resendTimer = Timer.periodic(const Duration(seconds: 1), (t) {
      if (!mounted) { t.cancel(); return; }
      if (_resendSeconds <= 0) { t.cancel(); return; }
      setState(() => _resendSeconds--);
    });
  }

  // ── Verify OTP ───────────────────────────────────────────────────────────────
  Future<void> _verifyOtp() async {
    if (_otpCtrl.text.trim().length != 6) {
      setState(() => _error = 'Enter the 6-digit OTP');
      return;
    }
    setState(() { _loading = true; _error = ''; });
    final cred = PhoneAuthProvider.credential(
      verificationId: _verificationId,
      smsCode: _otpCtrl.text.trim(),
    );
    await _signIn(cred);
  }

  Future<void> _signIn(PhoneAuthCredential cred) async {
    try {
      await _auth.signInWithCredential(cred);
      // AuthGate stream detects sign-in, re-routes automatically
    } on FirebaseAuthException catch (e) {
      if (mounted) setState(() { _loading = false; _error = e.message ?? 'Sign-in failed'; });
    }
  }

  // ── Build ────────────────────────────────────────────────────────────────────
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.blue.shade50,
      body: SafeArea(
        child: Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.symmetric(horizontal: 28),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const SizedBox(height: 48),
                const _AppIcon(color: Colors.blue, icon: Icons.water_drop),
                const SizedBox(height: 20),
                Text(
                  'Pump Controller',
                  style: Theme.of(context).textTheme.headlineSmall?.copyWith(
                    fontWeight: FontWeight.bold,
                    color: Colors.blue.shade800,
                  ),
                ),
                const SizedBox(height: 6),
                Text(
                  _otpSent
                      ? 'Enter the OTP sent to your phone'
                      : 'Sign in with your mobile number',
                  style: TextStyle(color: Colors.grey.shade600, fontSize: 14),
                  textAlign: TextAlign.center,
                ),
                const SizedBox(height: 36),
                if (!_otpSent) _buildPhoneStep(),
                if (_otpSent)  _buildOtpStep(),
                if (_error.isNotEmpty) _ErrorBox(message: _error),
                const SizedBox(height: 48),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildPhoneStep() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        TextField(
          controller: _phoneCtrl,
          keyboardType: TextInputType.phone,
          maxLength: 10,
          inputFormatters: [FilteringTextInputFormatter.digitsOnly],
          onSubmitted: (_) => _sendOtp(),
          decoration: InputDecoration(
            counterText: '',
            labelText: 'Mobile Number',
            prefixIcon: const _PrefixLabel('+91'),
            border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
            filled: true,
            fillColor: Colors.white,
          ),
        ),
        const SizedBox(height: 20),
        _ActionButton(
          label: 'Send OTP',
          icon: Icons.sms,
          color: Colors.blue,
          loading: _loading,
          onTap: _sendOtp,
        ),
      ],
    );
  }

  Widget _buildOtpStep() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(
          'OTP sent to +91\u00a0${_phoneCtrl.text.trim()}',
          textAlign: TextAlign.center,
          style: TextStyle(color: Colors.blue.shade700, fontWeight: FontWeight.w500),
        ),
        const SizedBox(height: 16),
        TextField(
          controller: _otpCtrl,
          keyboardType: TextInputType.number,
          maxLength: 6,
          autofocus: true,
          textAlign: TextAlign.center,
          inputFormatters: [FilteringTextInputFormatter.digitsOnly],
          style: const TextStyle(
              fontSize: 26, letterSpacing: 10, fontWeight: FontWeight.bold),
          onSubmitted: (_) => _verifyOtp(),
          decoration: InputDecoration(
            counterText: '',
            labelText: 'OTP',
            border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
            filled: true,
            fillColor: Colors.white,
          ),
        ),
        const SizedBox(height: 20),
        _ActionButton(
          label: 'Verify OTP',
          icon: Icons.verified,
          color: Colors.blue,
          loading: _loading,
          onTap: _verifyOtp,
        ),
        const SizedBox(height: 8),
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            TextButton(
              onPressed: _resendSeconds > 0
                  ? null
                  : () => _sendOtp(resend: true),
              child: Text(
                _resendSeconds > 0
                    ? 'Resend in ${_resendSeconds}s'
                    : 'Resend OTP',
              ),
            ),
            Text('·', style: TextStyle(color: Colors.grey.shade400)),
            TextButton(
              onPressed: () => setState(() {
                _otpSent = false;
                _otpCtrl.clear();
                _error = '';
              }),
              child: const Text('Change number'),
            ),
          ],
        ),
      ],
    );
  }
}

// ─── DeviceCodeScreen ─────────────────────────────────────────────────────────
// Shown to a newly authenticated user who has no sites assigned yet.
// Admin creates pairing codes in Firebase:
//   pairing_codes/{CODE} = { site_id: "site01", claimed: false }
// On success: writes user_sites/{uid} = ['site01'] — _SiteGate auto-navigates.
class DeviceCodeScreen extends StatefulWidget {
  final String uid;
  const DeviceCodeScreen({super.key, required this.uid});
  @override
  State<DeviceCodeScreen> createState() => _DeviceCodeScreenState();
}

class _DeviceCodeScreenState extends State<DeviceCodeScreen> {
  final _codeCtrl = TextEditingController();
  bool   _loading = false;
  String _error   = '';

  @override
  void dispose() { _codeCtrl.dispose(); super.dispose(); }

  Future<void> _link() async {
    final code = _codeCtrl.text.trim().toUpperCase();
    if (code.isEmpty) {
      setState(() => _error = 'Enter the pairing code');
      return;
    }
    setState(() { _loading = true; _error = ''; });

    final db   = FirebaseDatabase.instance;
    final snap = await db.ref('pairing_codes/$code').get();

    if (!snap.exists) {
      setState(() {
        _loading = false;
        _error = 'Invalid pairing code. Check with your administrator.';
      });
      return;
    }

    final data = Map<String, dynamic>.from(snap.value as Map);

    if (data['claimed'] == true) {
      setState(() {
        _loading = false;
        _error = 'This code is already used. Ask your administrator for a new one.';
      });
      return;
    }

    final siteId = (data['site_id'] as String?) ?? '';
    if (siteId.isEmpty) {
      setState(() { _loading = false; _error = 'Invalid code configuration.'; });
      return;
    }

    // Claim the code, then assign site to user
    await db.ref('pairing_codes/$code').update({
      'claimed':    true,
      'claimed_by': widget.uid,
      'claimed_at': ServerValue.timestamp,
    });
    await db.ref('user_sites/${widget.uid}').set([siteId]);
    // _SiteGate's StreamBuilder fires on user_sites write → auto-navigates to dashboard
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.blue.shade50,
      body: SafeArea(
        child: Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.symmetric(horizontal: 28),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const SizedBox(height: 48),
                const _AppIcon(color: Colors.orange, icon: Icons.qr_code_scanner),
                const SizedBox(height: 20),
                Text(
                  'Link Your Device',
                  style: Theme.of(context).textTheme.headlineSmall?.copyWith(
                    fontWeight: FontWeight.bold,
                    color: Colors.blue.shade800,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  'Enter the pairing code provided by your administrator\nto link this account to your pump site.',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: Colors.grey.shade600, fontSize: 14),
                ),
                const SizedBox(height: 36),
                TextField(
                  controller: _codeCtrl,
                  textCapitalization: TextCapitalization.characters,
                  textAlign: TextAlign.center,
                  maxLength: 8,
                  inputFormatters: [
                    FilteringTextInputFormatter.allow(RegExp(r'[a-zA-Z0-9]')),
                  ],
                  style: const TextStyle(
                      fontSize: 26, letterSpacing: 6, fontWeight: FontWeight.bold),
                  onSubmitted: (_) => _link(),
                  decoration: InputDecoration(
                    counterText: '',
                    labelText: 'Pairing Code',
                    hintText: 'e.g. SITE01AB',
                    border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
                    filled: true,
                    fillColor: Colors.white,
                  ),
                ),
                const SizedBox(height: 20),
                _ActionButton(
                  label: 'Link Device',
                  icon: Icons.link,
                  color: Colors.orange,
                  loading: _loading,
                  onTap: _link,
                ),
                if (_error.isNotEmpty) _ErrorBox(message: _error),
                const SizedBox(height: 24),
                TextButton.icon(
                  icon: const Icon(Icons.logout, size: 16),
                  label: const Text('Sign out'),
                  onPressed: () => FirebaseAuth.instance.signOut(),
                  style: TextButton.styleFrom(foregroundColor: Colors.grey.shade600),
                ),
                const SizedBox(height: 48),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

// ─── Shared small widgets ─────────────────────────────────────────────────────

class _AppIcon extends StatelessWidget {
  final Color    color;
  final IconData icon;
  const _AppIcon({required this.color, required this.icon});
  @override
  Widget build(BuildContext context) {
    return Container(
      width: 80, height: 80,
      decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(20)),
      child: Icon(icon, color: Colors.white, size: 44),
    );
  }
}

class _PrefixLabel extends StatelessWidget {
  final String text;
  const _PrefixLabel(this.text);
  @override
  Widget build(BuildContext context) {
    return Container(
      width: 60,
      alignment: Alignment.center,
      child: Text(text,
          style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w500)),
    );
  }
}

class _ActionButton extends StatelessWidget {
  final String   label;
  final IconData icon;
  final Color    color;
  final bool     loading;
  final VoidCallback onTap;

  const _ActionButton({
    required this.label,
    required this.icon,
    required this.color,
    required this.loading,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 50,
      child: ElevatedButton.icon(
        icon: loading
            ? const SizedBox(
                width: 18, height: 18,
                child: CircularProgressIndicator(color: Colors.white, strokeWidth: 2),
              )
            : Icon(icon),
        label: Text(label, style: const TextStyle(fontSize: 16)),
        onPressed: loading ? null : onTap,
        style: ElevatedButton.styleFrom(
          backgroundColor: color,
          foregroundColor: Colors.white,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
        ),
      ),
    );
  }
}

class _ErrorBox extends StatelessWidget {
  final String message;
  const _ErrorBox({required this.message});
  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 16),
      child: Container(
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(
          color: Colors.red.shade50,
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: Colors.red.shade200),
        ),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Icon(Icons.error_outline, color: Colors.red.shade600, size: 18),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                message,
                style: TextStyle(color: Colors.red.shade700, fontSize: 13),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
