// lib/main.dart
// Flutter pump controller app
// Reads pump/status from Firebase, writes pump/cmd

import 'dart:async';
import 'package:flutter/material.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_database/firebase_database.dart';
import 'auth_screen.dart';

// ─── Site configuration ───────────────────────────────────────────────────────
class SiteConfig {
  final String id;
  final String name;
  final String meterPumpId; // pump whose status feeds PowerMeterCard
  final String deviceId;    // physical STM32 device that owns relay1+relay2 for this site
  final List<String> pumpIds; // index 0 → relay1, index 1 → relay2 on deviceId

  const SiteConfig({
    required this.id,
    required this.name,
    required this.meterPumpId,
    required this.deviceId,
    required this.pumpIds,
  });
}

const kSites = [
  SiteConfig(
    id: 'site01',
    name: 'Site 1',
    meterPumpId: 'pump01',
    deviceId: 'pump01',        // pump01 device owns relay1 (pump01) and relay2 (pump02)
    pumpIds: ['pump01', 'pump02'],
  ),
  SiteConfig(
    id: 'site02',
    name: 'Site 2',
    meterPumpId: 'pump03',
    deviceId: 'pump03',        // future separate device
    pumpIds: ['pump03', 'pump04'],
  ),
];

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: const FirebaseOptions(
      apiKey:            "AIzaSyAKRv98QPE4FraRgGypvdvJfCG0RQs97O0",
      appId:             "1:22800348697:android:89f6c77fdb6492a797dc88",
      messagingSenderId: "22800348697",
      projectId:         "pump-controller-4398d",
      databaseURL:       "https://pump-controller-4398d-default-rtdb.firebaseio.com",
    ),
  );
  runApp(const PumpApp());
}

class PumpApp extends StatelessWidget {
  const PumpApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Pump Controller',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: AuthGate(
        dashboardBuilder: (siteIds) => PumpDashboard(allowedSiteIds: siteIds),
      ),
    );
  }
}

// ─── Dashboard — mutual exclusion coordinator ─────────────────────────────────
class PumpDashboard extends StatefulWidget {
  final List<String> allowedSiteIds;
  const PumpDashboard({super.key, required this.allowedSiteIds});
  @override
  State<PumpDashboard> createState() => _PumpDashboardState();
}

class _PumpDashboardState extends State<PumpDashboard> {
  final db = FirebaseDatabase.instance;

  late final List<SiteConfig> _sites;
  final Map<String, bool?> _pumpOn = {};

  @override
  void initState() {
    super.initState();
    _sites = kSites.where((s) => widget.allowedSiteIds.contains(s.id)).toList();
    for (final site in _sites) {
      for (var i = 0; i < site.pumpIds.length; i++) {
        final pumpId   = site.pumpIds[i];
        final relayNum = i + 1; // relay1 for index 0, relay2 for index 1
        // Status is published by the physical device (site.deviceId), not per logical pump
        db.ref('pumps/${site.deviceId}/status/relay${relayNum}_state').onValue.listen((event) {
          if (mounted) setState(() => _pumpOn[pumpId] = (event.snapshot.value ?? 0) == 1);
        });
      }
    }
  }

  Future<void> _handlePumpToggle(String pumpId, bool turnOn) async {
    // Each pump is its own device — always relay1
    await db.ref('pumps/$pumpId/cmd').set({
      'relay1': turnOn ? 1 : 0,
      'ts': DateTime.now().millisecondsSinceEpoch,
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Pump Controller'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          IconButton(
            icon: const Icon(Icons.history),
            tooltip: 'Logs',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(builder: (_) => LogsPage(
                pumpIds: _sites.expand((s) => s.pumpIds).toList(),
              )),
            ),
          ),
          IconButton(
            icon: const Icon(Icons.settings),
            tooltip: 'Protection Settings',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(builder: (_) => SettingsPage(
                pumpIds: _sites.expand((s) => s.pumpIds).toList(),
              )),
            ),
          ),
          IconButton(
            icon: const Icon(Icons.logout),
            tooltip: 'Sign out',
            onPressed: () => FirebaseAuth.instance.signOut(),
          ),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            for (final site in _sites)
              _SiteSection(
                site: site,
                pumpOn: _pumpOn,
                onPumpToggle: _handlePumpToggle,
                showHeader: _sites.length > 1,
              ),
          ],
        ),
      ),
    );
  }
}

// ─── Site section — groups PowerMeter + Pumps + Rotation for one site ─────────
class _SiteSection extends StatelessWidget {
  final SiteConfig site;
  final Map<String, bool?> pumpOn;
  final Future<void> Function(String pumpId, bool on) onPumpToggle;
  final bool showHeader;

  const _SiteSection({
    required this.site,
    required this.pumpOn,
    required this.onPumpToggle,
    required this.showHeader,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        if (showHeader)
          Padding(
            padding: const EdgeInsets.fromLTRB(4, 8, 4, 4),
            child: Text(
              site.name,
              style: Theme.of(context)
                  .textTheme
                  .titleMedium
                  ?.copyWith(fontWeight: FontWeight.bold),
            ),
          ),
        PowerMeterCard(pumpId: site.meterPumpId),
        const SizedBox(height: 16),
        for (var i = 0; i < site.pumpIds.length; i++) ...[
          PumpCard(
            pumpId: site.pumpIds[i],
            pumpName: 'Pump ${i + 1}',
            deviceId: site.deviceId,   // physical device — status lives at pumps/{deviceId}/status
            relayNum: i + 1,           // relay1 for pump[0], relay2 for pump[1]
            otherPumpOn: site.pumpIds
                .where((p) => p != site.pumpIds[i])
                .any((p) => pumpOn[p] == true),
            otherPumpName: 'Pump ${i == 0 ? 2 : 1}',
            onPumpToggle: (val) => onPumpToggle(site.pumpIds[i], val),
          ),
          const SizedBox(height: 16),
        ],
        RotationScheduleCard(
          siteId: site.id,
          pump1Id: site.pumpIds[0],
          pump2Id: site.pumpIds[1],
        ),
      ],
    );
  }
}

// ─── Rotation schedule card ───────────────────────────────────────────────────
class RotationScheduleCard extends StatefulWidget {
  final String siteId;
  final String pump1Id;
  final String pump2Id;

  const RotationScheduleCard({
    super.key,
    required this.siteId,
    required this.pump1Id,
    required this.pump2Id,
  });
  @override
  State<RotationScheduleCard> createState() => _RotationScheduleCardState();
}

class _RotationScheduleCardState extends State<RotationScheduleCard> {
  final db = FirebaseDatabase.instance;

  bool   _enabled         = false;
  int    _intervalMinutes = 240; // default 4 h
  late String _currentPump;
  int    _startedAt       = 0;
  bool   _expanded        = false;

  static const _options = [
    (label: '30 min',  minutes: 30),
    (label: '1 hour',  minutes: 60),
    (label: '2 hours', minutes: 120),
    (label: '3 hours', minutes: 180),
    (label: '4 hours', minutes: 240),
    (label: '6 hours', minutes: 360),
    (label: '8 hours', minutes: 480),
    (label: '12 hours',minutes: 720),
  ];

  @override
  void initState() {
    super.initState();
    _currentPump = widget.pump1Id;
    db.ref('sites/${widget.siteId}/rotation_schedule').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _enabled         = s['enabled']          ?? false;
          _intervalMinutes = (s['interval_minutes'] ?? 240) as int;
          _currentPump     = s['current_pump']      ?? widget.pump1Id;
          _startedAt       = (s['started_at']       ?? 0) as int;
        });
      }
    });
  }

  String _timeRemaining() {
    if (!_enabled || _startedAt == 0) return '';
    final elapsedMs  = DateTime.now().millisecondsSinceEpoch - _startedAt;
    final intervalMs = _intervalMinutes * 60 * 1000;
    final remainMs   = intervalMs - elapsedMs;
    if (remainMs <= 0) return 'Switching soon...';
    final h = remainMs ~/ 3600000;
    final m = (remainMs % 3600000) ~/ 60000;
    return h > 0 ? '${h}h ${m}m remaining' : '${m}m remaining';
  }

  Future<void> _save() async {
    await db.ref('sites/${widget.siteId}/rotation_schedule').update({
      'enabled':          _enabled,
      'interval_minutes': _intervalMinutes,
    });
    if (!_enabled) {
      // clear started_at so it restarts cleanly when re-enabled
      await db.ref('sites/${widget.siteId}/rotation_schedule/started_at').set(0);
    }
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Rotation schedule saved'),
            duration: Duration(seconds: 2)));
    }
  }

  @override
  Widget build(BuildContext context) {
    final activePumpLabel = _currentPump == widget.pump1Id ? 'Pump 1' : 'Pump 2';
    final remaining       = _timeRemaining();

    return Card(
      elevation: 2,
      color: Colors.teal.shade50,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // ── Header row ──────────────────────────────────────────────────
            InkWell(
              onTap: () => setState(() => _expanded = !_expanded),
              child: Row(
                children: [
                  const Icon(Icons.autorenew, color: Colors.teal),
                  const SizedBox(width: 8),
                  const Text('Pump Rotation',
                      style: TextStyle(
                          fontSize: 16,
                          fontWeight: FontWeight.bold,
                          color: Colors.teal)),
                  const Spacer(),
                  if (_enabled) ...[
                    Container(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 8, vertical: 2),
                      decoration: BoxDecoration(
                        color: Colors.teal,
                        borderRadius: BorderRadius.circular(10),
                      ),
                      child: Text('$activePumpLabel active',
                          style: const TextStyle(
                              color: Colors.white, fontSize: 11)),
                    ),
                    const SizedBox(width: 6),
                  ],
                  Icon(_expanded ? Icons.expand_less : Icons.expand_more,
                      color: Colors.teal),
                ],
              ),
            ),

            if (_enabled && remaining.isNotEmpty) ...[
              const SizedBox(height: 4),
              Text(remaining,
                  style: TextStyle(fontSize: 12, color: Colors.teal.shade700)),
            ],

            if (_expanded) ...[
              const SizedBox(height: 12),
              // Enable toggle
              Row(
                children: [
                  const Text('Enable rotation'),
                  const Spacer(),
                  Switch(
                    value: _enabled,
                    activeThumbColor: Colors.teal,
                    onChanged: (v) => setState(() => _enabled = v),
                  ),
                ],
              ),
              // Interval picker
              Row(
                children: [
                  const Text('Switch every'),
                  const Spacer(),
                  DropdownButton<int>(
                    value: _intervalMinutes,
                    items: _options
                        .map((o) => DropdownMenuItem(
                              value: o.minutes,
                              child: Text(o.label),
                            ))
                        .toList(),
                    onChanged: (v) =>
                        setState(() => _intervalMinutes = v ?? 240),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              SizedBox(
                width: double.infinity,
                child: ElevatedButton.icon(
                  icon: const Icon(Icons.save, size: 16),
                  label: const Text('Save Rotation'),
                  style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.teal,
                      foregroundColor: Colors.white),
                  onPressed: _save,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

// ─── Individual pump card ─────────────────────────────────────────────────────
class PumpCard extends StatefulWidget {
  final String pumpId;
  final String pumpName;
  final String deviceId;
  final int    relayNum;
  final bool otherPumpOn;
  final String otherPumpName;
  final Future<void> Function(bool) onPumpToggle;

  const PumpCard({
    super.key,
    required this.pumpId,
    required this.pumpName,
    required this.deviceId,
    required this.relayNum,
    required this.otherPumpOn,
    required this.otherPumpName,
    required this.onPumpToggle,
  });

  @override
  State<PumpCard> createState() => _PumpCardState();
}

class _PumpCardState extends State<PumpCard> {
  final db = FirebaseDatabase.instance;

  Map<String, dynamic> _alerts = {};
  bool? _relay1Cmd;   // null = waiting for Firebase data
  bool _isRunning = false;

  // Schedule state
  bool      _schedExpanded = false;
  bool      _schedEnabled  = false;
  TimeOfDay _schedOnTime   = const TimeOfDay(hour: 8,  minute: 0);
  TimeOfDay _schedOffTime  = const TimeOfDay(hour: 18, minute: 0);

  @override
  void initState() {
    super.initState();
    _listenStatus();
    _listenAlerts();
    _listenSchedule();
  }

  void _listenStatus() {
    db.ref('pumps/${widget.deviceId}/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _relay1Cmd = (s['relay${widget.relayNum}_state'] ?? 0) == 1;
          _isRunning = (s['relay${widget.relayNum}_running'] ?? 0) == 1;
        });
      }
    });
  }

  void _listenAlerts() {
    db.ref('pumps/${widget.pumpId}/alerts').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        setState(() => _alerts = Map<String, dynamic>.from(data as Map));
      }
    });
  }

  void _listenSchedule() {
    db.ref('pumps/${widget.pumpId}/schedule').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        final s = Map<String, dynamic>.from(data as Map);
        setState(() {
          _schedEnabled = s['enabled'] ?? false;
          _schedOnTime  = TimeOfDay(
              hour: (s['on_hour']  ?? 8)  as int,
              minute: (s['on_min']  ?? 0) as int);
          _schedOffTime = TimeOfDay(
              hour: (s['off_hour'] ?? 18) as int,
              minute: (s['off_min'] ?? 0) as int);
        });
      }
    });
  }

  Future<void> _saveSchedule() async {
    // Conflict check: prevent same-site pumps having the same ON time
    if (_schedEnabled) {
      final site = kSites.firstWhere(
        (s) => s.pumpIds.contains(widget.pumpId),
        orElse: () => kSites.first,
      );
      for (final otherId in site.pumpIds.where((p) => p != widget.pumpId)) {
        final otherSnap = await db.ref('pumps/$otherId/schedule').get();
        if (otherSnap.exists) {
          final other = Map<String, dynamic>.from(otherSnap.value as Map);
          final otherEnabled = other['enabled'] ?? false;
          if (otherEnabled) {
            final otherOnHour = (other['on_hour'] ?? 8) as int;
            final otherOnMin  = (other['on_min']  ?? 0) as int;
            if (otherOnHour == _schedOnTime.hour &&
                otherOnMin  == _schedOnTime.minute) {
              if (mounted) {
                final otherIdx = site.pumpIds.indexOf(otherId);
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(
                    content: Text(
                      'Conflict: Pump ${otherIdx + 1} '
                      'already turns ON at ${_fmt(_schedOnTime)}. '
                      'Choose a different time.',
                    ),
                    backgroundColor: Colors.red.shade700,
                    duration: const Duration(seconds: 4),
                  ),
                );
              }
              return; // block save
            }
          }
        }
      }
    }

    await db.ref('pumps/${widget.pumpId}/schedule').set({
      'enabled':  _schedEnabled,
      'on_hour':  _schedOnTime.hour,
      'on_min':   _schedOnTime.minute,
      'off_hour': _schedOffTime.hour,
      'off_min':  _schedOffTime.minute,
    });
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Schedule saved'),
          duration: Duration(seconds: 2),
        ),
      );
    }
  }

  Future<void> _pickTime(bool isOnTime) async {
    final picked = await showTimePicker(
      context: context,
      initialTime: isOnTime ? _schedOnTime : _schedOffTime,
    );
    if (picked != null && mounted) {
      setState(() {
        if (isOnTime) { _schedOnTime  = picked; }
        else          { _schedOffTime = picked; }
      });
    }
  }

  String _fmt(TimeOfDay t) =>
      '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context) {
    final bool alertOV  = _alerts['overvoltage']  ?? false;
    final bool alertUV  = _alerts['undervoltage'] ?? false;
    final bool alertPL  = _alerts['phase_loss']   ?? false;
    final bool alertDR  = _alerts['dry_run_trip'] ?? false;
    final bool anyAlert = alertOV || alertUV || alertPL || alertDR;

    // Resolve nullable relay state: null = waiting for Firebase data
    final bool loading  = _relay1Cmd == null;
    final bool relayOn  = _relay1Cmd == true;
    final Color stateColor = loading
        ? Colors.grey
        : relayOn
            ? (_isRunning ? Colors.green : Colors.orange)
            : Colors.red;
    final String stateText = loading
        ? '---'
        : relayOn
            ? (_isRunning ? 'RUNNING' : 'STARTING...')
            : 'STOPPED';

    return Card(
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [

            // ── Header ───────────────────────────────────────────────────────
            Text(widget.pumpName,
                style: const TextStyle(
                    fontSize: 20, fontWeight: FontWeight.bold)),
            const Divider(),

            // ── Pump status icon ──────────────────────────────────────────────
            const SizedBox(height: 8),
            Center(
              child: Column(
                children: [
                  Container(
                    padding: const EdgeInsets.all(20),
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: stateColor.withValues(alpha: 0.1),
                      border: Border.all(color: stateColor, width: 2.5),
                    ),
                    child: Icon(
                      relayOn ? Icons.water_drop : Icons.water_drop_outlined,
                      size: 52,
                      color: stateColor,
                    ),
                  ),
                  const SizedBox(height: 6),
                  Text(
                    stateText,
                    style: TextStyle(
                      fontSize: 13,
                      fontWeight: FontWeight.bold,
                      color: stateColor,
                      letterSpacing: 1,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 12),

            // ── Alerts ────────────────────────────────────────────────────────
            if (anyAlert) ...[
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(10),
                decoration: BoxDecoration(
                  color: Colors.red.shade50,
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: Colors.red.shade200),
                ),
                child: Wrap(
                  spacing: 8,
                  children: [
                    if (alertOV) const _AlertChip(label: 'Overvoltage'),
                    if (alertUV) const _AlertChip(label: 'Undervoltage'),
                    if (alertPL) const _AlertChip(label: 'Phase Loss'),
                    if (alertDR) const _AlertChip(label: 'Dry Run Trip'),
                  ],
                ),
              ),
              const SizedBox(height: 12),
            ],

            // ── Pump button ───────────────────────────────────────────────────
            const Text('Relay Control',
                style: TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 8),
            SizedBox(
              width: double.infinity,
              child: _RelayButton(
                label: 'Pump',
                isOn: relayOn,
                disabled: loading || (widget.otherPumpOn && !relayOn),
                onToggle: (val) {
                  setState(() => _relay1Cmd = val);
                  widget.onPumpToggle(val);
                },
              ),
            ),
            if (widget.otherPumpOn && !relayOn && !loading) ...[
              const SizedBox(height: 6),
              Row(
                children: [
                  const Icon(Icons.info_outline, size: 14, color: Colors.orange),
                  const SizedBox(width: 4),
                  Text(
                    'Turn off ${widget.otherPumpName} first',
                    style: const TextStyle(fontSize: 12, color: Colors.orange),
                  ),
                ],
              ),
            ],
            const SizedBox(height: 12),

            // ── Schedule section ──────────────────────────────────────────────
            InkWell(
              onTap: () => setState(() => _schedExpanded = !_schedExpanded),
              borderRadius: BorderRadius.circular(8),
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                decoration: BoxDecoration(
                  color: Colors.blue.shade50,
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: Colors.blue.shade200),
                ),
                child: Row(
                  children: [
                    const Icon(Icons.schedule, size: 18, color: Colors.blue),
                    const SizedBox(width: 6),
                    const Text('Schedule',
                        style: TextStyle(fontWeight: FontWeight.w600, color: Colors.blue)),
                    const Spacer(),
                    if (_schedEnabled)
                      Text('${_fmt(_schedOnTime)} – ${_fmt(_schedOffTime)}',
                          style: const TextStyle(fontSize: 12, color: Colors.blue)),
                    const SizedBox(width: 6),
                    Icon(
                      _schedExpanded ? Icons.expand_less : Icons.expand_more,
                      color: Colors.blue,
                    ),
                  ],
                ),
              ),
            ),

            if (_schedExpanded) ...[
              const SizedBox(height: 10),
              // Enable toggle
              Row(
                children: [
                  const Text('Enable schedule'),
                  const Spacer(),
                  Switch(
                    value: _schedEnabled,
                    onChanged: (v) => setState(() => _schedEnabled = v),
                  ),
                ],
              ),
              if (_schedEnabled) ...[
                const SizedBox(height: 6),
                // ON time row
                Row(
                  children: [
                    const Icon(Icons.power_settings_new,
                        size: 16, color: Colors.green),
                    const SizedBox(width: 6),
                    const Text('Turn ON at'),
                    const Spacer(),
                    TextButton(
                      onPressed: () => _pickTime(true),
                      child: Text(_fmt(_schedOnTime),
                          style: const TextStyle(
                              fontSize: 16, fontWeight: FontWeight.bold)),
                    ),
                  ],
                ),
                // OFF time row
                Row(
                  children: [
                    const Icon(Icons.power_off, size: 16, color: Colors.red),
                    const SizedBox(width: 6),
                    const Text('Turn OFF at'),
                    const Spacer(),
                    TextButton(
                      onPressed: () => _pickTime(false),
                      child: Text(_fmt(_schedOffTime),
                          style: const TextStyle(
                              fontSize: 16, fontWeight: FontWeight.bold)),
                    ),
                  ],
                ),
              ],
              const SizedBox(height: 4),
              SizedBox(
                width: double.infinity,
                child: ElevatedButton.icon(
                  icon: const Icon(Icons.save, size: 16),
                  label: const Text('Save Schedule'),
                  onPressed: _saveSchedule,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

// ─── Shared power meter card ─────────────────────────────────────────────────
class PowerMeterCard extends StatefulWidget {
  final String pumpId;
  const PowerMeterCard({super.key, required this.pumpId});
  @override
  State<PowerMeterCard> createState() => _PowerMeterCardState();
}

class _PowerMeterCardState extends State<PowerMeterCard> {
  final db = FirebaseDatabase.instance;
  Map<String, dynamic> _status = {};

  @override
  void initState() {
    super.initState();
    db.ref('pumps/${widget.pumpId}/status').onValue.listen((event) {
      final data = event.snapshot.value;
      if (data != null && mounted) {
        setState(() => _status = Map<String, dynamic>.from(data as Map));
      }
    });
  }

  int _rssiToDbm(int rssi) => rssi == 99 ? 0 : -113 + rssi * 2;

  IconData _signalIcon(int rssi) {
    if (rssi == 99 || rssi < 6) return Icons.signal_cellular_0_bar;
    if (rssi < 15)              return Icons.signal_cellular_alt;
    return                             Icons.signal_cellular_4_bar;
  }

  Color _signalColor(int rssi) {
    if (rssi == 99 || rssi < 6) return Colors.grey;
    if (rssi < 10)              return Colors.red;
    if (rssi < 15)              return Colors.orange;
    return Colors.green;
  }

  @override
  Widget build(BuildContext context) {
    final double v1      = (_status['v1']      ?? 0.0).toDouble();
    final double v2      = (_status['v2']      ?? 0.0).toDouble();
    final double v3      = (_status['v3']      ?? 0.0).toDouble();
    final double current = (_status['current'] ?? 0.0).toDouble();
    final bool isOnline  = _status['online']   ?? false;
    final int rssi       = (_status['rssi']    ?? 99) as int;
    final Color onlineColor = isOnline ? Colors.green : Colors.red;

    return Card(
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // Header: title + signal + online
            Row(
              children: [
                const Icon(Icons.electric_meter, color: Colors.orange),
                const SizedBox(width: 8),
                const Text('Power Meter',
                    style: TextStyle(
                        fontSize: 16, fontWeight: FontWeight.bold)),
                const Spacer(),
                Icon(_signalIcon(rssi),
                    color: _signalColor(rssi), size: 18),
                const SizedBox(width: 2),
                Text(
                  rssi == 99 ? '—' : '${_rssiToDbm(rssi)} dBm',
                  style: TextStyle(fontSize: 11, color: _signalColor(rssi)),
                ),
                const SizedBox(width: 10),
                Icon(Icons.circle, color: onlineColor, size: 12),
                const SizedBox(width: 4),
                Text(isOnline ? 'Online' : 'Offline',
                    style: TextStyle(color: onlineColor, fontSize: 13)),
              ],
            ),
            const Divider(),
            const Text('3-Phase Voltage',
                style: TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceAround,
              children: [
                _VoltageChip(label: 'L1', voltage: v1),
                _VoltageChip(label: 'L2', voltage: v2),
                _VoltageChip(label: 'L3', voltage: v3),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                const Icon(Icons.bolt, size: 18, color: Colors.orange),
                const SizedBox(width: 4),
                Text('Current: ${current.toStringAsFixed(2)} A',
                    style: const TextStyle(fontSize: 15)),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

// ─── Voltage chip widget ──────────────────────────────────────────────────────
class _VoltageChip extends StatelessWidget {
  final String label;
  final double voltage;
  const _VoltageChip({required this.label, required this.voltage});

  Color _color() {
    if (voltage > 460 || voltage < 360) return Colors.red;
    if (voltage < 390 || voltage > 440) return Colors.orange;
    return Colors.green;
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      decoration: BoxDecoration(
        color:  _color().withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _color().withValues(alpha: 0.4)),
      ),
      child: Column(
        children: [
          Text(label,
              style: TextStyle(color: _color(), fontWeight: FontWeight.bold)),
          Text('${voltage.toStringAsFixed(1)}V',
              style: TextStyle(color: _color(), fontSize: 13)),
        ],
      ),
    );
  }
}

// ─── Alert chip ───────────────────────────────────────────────────────────────
class _AlertChip extends StatelessWidget {
  final String label;
  const _AlertChip({required this.label});

  @override
  Widget build(BuildContext context) {
    return Chip(
      label: Text(label,
          style: const TextStyle(color: Colors.red, fontSize: 12)),
      backgroundColor: Colors.red.shade100,
      side: BorderSide(color: Colors.red.shade300),
      padding: EdgeInsets.zero,
    );
  }
}

// ─── Protection Settings page ─────────────────────────────────────────────────
class SettingsPage extends StatefulWidget {
  final List<String> pumpIds;
  const SettingsPage({super.key, required this.pumpIds});
  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final db = FirebaseDatabase.instance;
  final _formKey = GlobalKey<FormState>();

  final _ovCtrl     = TextEditingController();
  final _uvCtrl     = TextEditingController();
  final _plCtrl     = TextEditingController();
  final _dryICtrl   = TextEditingController();
  final _dryTCtrl   = TextEditingController();
  final _startTCtrl = TextEditingController();

  late String _pumpId;  // selected pump
  int?   _selectedHp;               // null=custom, 5=5HP, 75=7.5HP
  bool   _dryRunEnabled = true;     // dry run optional

  static const _hpPresets = {
    5:  {'ov': 480.0, 'uv': 360.0, 'pl': 200.0, 'dry_i': 3.0, 'dry_t': 8},
    75: {'ov': 480.0, 'uv': 360.0, 'pl': 200.0, 'dry_i': 4.5, 'dry_t': 8},
  };

  bool _loading = true;
  bool _saving  = false;

  double? _devOv, _devUv, _devPl, _devDryI;
  int?    _devDryT, _devStartT, _devHp, _devDryEn;

  // Relay2-specific "Active on device" values (pump02 only)
  double? _devDryI2;
  int?    _devDryT2, _devStartT2, _devHp2, _devDryEn2;

  StreamSubscription<DatabaseEvent>? _devSub;

  @override
  void initState() {
    super.initState();
    _pumpId = widget.pumpIds.isNotEmpty ? widget.pumpIds.first : 'pump01';
    _load();
    _listenDeviceSettings();
  }

  // The physical STM32 device that owns this pump's relay.
  // pump01 → pump01 device; pump02 → pump01 device (relay2 on same board).
  String get _deviceId => kSites
      .firstWhere((s) => s.pumpIds.contains(_pumpId),
          orElse: () => kSites.first)
      .deviceId;

  // true when the selected pump is relay2 on its site (e.g. pump02 on site01).
  bool get _isRelay2 {
    final site = kSites.firstWhere((s) => s.pumpIds.contains(_pumpId),
        orElse: () => kSites.first);
    return site.pumpIds.indexOf(_pumpId) == 1;
  }

  // pump01 → pumps/pump01/settings (ov/uv/pl/dry_i/…)
  // pump02 → pumps/pump02/settings (dry_i/dry_t/dry_en/hp only)
  String get _settingsPath => 'pumps/$_pumpId/settings';
  // Live cfg values always come from the physical device's status.
  String get _statusPath   => 'pumps/$_deviceId/status';

  Future<void> _load() async {
    setState(() => _loading = true);
    final snap = await db.ref(_settingsPath).get();
    final s = snap.value as Map?;
    setState(() {
      _selectedHp    = (s?['hp']     as num?)?.toInt();
      // null dry_en → default true; 0 → false
      _dryRunEnabled = (s?['dry_en'] as num?)?.toInt() != 0;
      _ovCtrl.text   = (s?['ov']    ?? 480).toString();
      _uvCtrl.text   = (s?['uv']    ?? 360).toString();
      _plCtrl.text   = (s?['pl']    ?? 200).toString();
      _dryICtrl.text   = (s?['dry_i']   ?? 1.5).toString();
      _dryTCtrl.text   = (s?['dry_t']   ?? 8).toString();
      _startTCtrl.text = (s?['start_t'] ?? 300).toString();
      _loading = false;
    });
  }

  void _listenDeviceSettings() {
    _devSub?.cancel();
    _devSub = db.ref(_statusPath).onValue.listen((event) {
      final s = event.snapshot.value as Map?;
      if (s == null || !mounted) return;
      setState(() {
        if (_isRelay2) {
          // pump02: read relay2-specific cfg fields published in pump01 status
          _devDryI2   = (s['cfg_dry_i2']   as num?)?.toDouble();
          _devDryT2   = (s['cfg_dry_t2']   as num?)?.toInt();
          _devStartT2 = (s['cfg_start_t2'] as num?)?.toInt();
          _devHp2     = (s['cfg_hp2']      as num?)?.toInt();
          _devDryEn2  = (s['cfg_dry_en2']  as num?)?.toInt();
        } else {
          _devOv     = (s['cfg_ov']      as num?)?.toDouble();
          _devUv     = (s['cfg_uv']      as num?)?.toDouble();
          _devPl     = (s['cfg_pl']      as num?)?.toDouble();
          _devDryI   = (s['cfg_dry_i']   as num?)?.toDouble();
          _devDryT   = (s['cfg_dry_t']   as num?)?.toInt();
          _devStartT = (s['cfg_start_t'] as num?)?.toInt();
          _devHp     = (s['cfg_hp']      as num?)?.toInt();
          _devDryEn  = (s['cfg_dry_en']  as num?)?.toInt();
        }
      });
    });
  }

  void _onPumpChanged(String pump) {
    _devSub?.cancel();
    setState(() {
      _pumpId = pump;
      _loading = true;
      _devOv = _devUv = _devPl = _devDryI = null;
      _devDryT = _devStartT = _devHp = _devDryEn = null;
      _devDryI2 = null;
      _devDryT2 = _devStartT2 = _devHp2 = _devDryEn2 = null;
    });
    _load();
    _listenDeviceSettings();
  }

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;
    final ov   = double.parse(_ovCtrl.text);
    final uv   = double.parse(_uvCtrl.text);
    final pl   = double.parse(_plCtrl.text);
    final dryI   = _dryRunEnabled ? double.parse(_dryICtrl.text)   : 0.0;
    final dryT   = _dryRunEnabled ? int.parse(_dryTCtrl.text)      : 0;
    final startT = int.tryParse(_startTCtrl.text) ?? 90;

    setState(() => _saving = true);
    await db.ref(_settingsPath).set({
      // Voltage thresholds only apply to relay1 (shared power meter on site).
      // pump02 (relay2) skips ov/uv/pl — bridge routes to pump/02/settings
      // which firmware handles via apply_settings2().
      if (!_isRelay2) ...{'ov': ov, 'uv': uv, 'pl': pl},
      'dry_i': dryI, 'dry_t': dryT, 'start_t': startT,
      'dry_en': _dryRunEnabled ? 1 : 0,
      if (_selectedHp != null) 'hp': _selectedHp,
    });
    setState(() => _saving = false);
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Settings saved for Pump ${_pumpId.replaceAll('pump', '')}'),
      ));
    }
  }

  String? _validatePositive(String? v) {
    if (v == null || v.isEmpty) return 'Required';
    if (double.tryParse(v) == null || double.parse(v) <= 0) return 'Must be > 0';
    return null;
  }

  String? _validateOv(String? v) {
    final err = _validatePositive(v);
    if (err != null) return err;
    final uv = double.tryParse(_uvCtrl.text) ?? 0;
    if (double.parse(v!) <= uv) return 'Must be > undervoltage';
    return null;
  }

  String? _validateUv(String? v) {
    final err = _validatePositive(v);
    if (err != null) return err;
    final pl = double.tryParse(_plCtrl.text) ?? 0;
    if (double.parse(v!) <= pl) return 'Must be > phase loss';
    return null;
  }

  String? _validateInt(String? v) {
    if (v == null || v.isEmpty) return 'Required';
    if (int.tryParse(v) == null || int.parse(v) <= 0) return 'Must be a whole number > 0';
    return null;
  }

  Widget _field(String label, TextEditingController ctrl, String unit,
      String? Function(String?) validator, {String? hint}) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: TextFormField(
        controller: ctrl,
        decoration: InputDecoration(
          labelText: label,
          suffixText: unit,
          hintText: hint,
          border: const OutlineInputBorder(),
          isDense: true,
        ),
        keyboardType: const TextInputType.numberWithOptions(decimal: true),
        validator: validator,
      ),
    );
  }

  Widget _deviceRow(String label, String value) {
    return Row(
      children: [
        Text('$label: ', style: const TextStyle(fontSize: 12, color: Colors.grey)),
        Text(value, style: const TextStyle(fontSize: 12, color: Colors.blueGrey)),
      ],
    );
  }

  @override
  void dispose() {
    _devSub?.cancel();
    _ovCtrl.dispose(); _uvCtrl.dispose(); _plCtrl.dispose();
    _dryICtrl.dispose(); _dryTCtrl.dispose(); _startTCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Protection Settings'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : SingleChildScrollView(
              padding: const EdgeInsets.all(16),
              child: Form(
                key: _formKey,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    // ── Pump selection ──────────────────────────────────
                    Card(
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text('Pump Selection',
                                style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                            const SizedBox(height: 12),
                            SegmentedButton<String>(
                              segments: widget.pumpIds.map((id) {
                                final num = id.replaceAll('pump', '');
                                return ButtonSegment(value: id, label: Text('Pump $num'));
                              }).toList(),
                              selected: {_pumpId},
                              onSelectionChanged: (s) => _onPumpChanged(s.first),
                            ),
                          ],
                        ),
                      ),
                    ),
                    const SizedBox(height: 12),
                    // ── HP rating ───────────────────────────────────────
                    Card(
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text('Pump Rating',
                                style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                            const SizedBox(height: 12),
                            DropdownButtonFormField<int?>(
                              key: ValueKey(_selectedHp),
                              initialValue: _selectedHp,
                              decoration: const InputDecoration(
                                labelText: 'Select Rating',
                                border: OutlineInputBorder(),
                                isDense: true,
                              ),
                              items: const [
                                DropdownMenuItem(value: null, child: Text('Custom')),
                                DropdownMenuItem(value: 5,    child: Text('5 HP  (3.7 kW)')),
                                DropdownMenuItem(value: 75,   child: Text('7.5 HP  (5.6 kW)')),
                              ],
                              onChanged: (val) {
                                setState(() {
                                  _selectedHp = val;
                                  if (val != null && _hpPresets.containsKey(val)) {
                                    final p = _hpPresets[val]!;
                                    _ovCtrl.text   = p['ov'].toString();
                                    _uvCtrl.text   = p['uv'].toString();
                                    _plCtrl.text   = p['pl'].toString();
                                    _dryICtrl.text = p['dry_i'].toString();
                                    _dryTCtrl.text = p['dry_t'].toString();
                                  }
                                });
                              },
                            ),
                            const SizedBox(height: 6),
                            const Text(
                              'Selecting a rating fills preset thresholds. You can still edit values below.',
                              style: TextStyle(fontSize: 11, color: Colors.grey),
                            ),
                          ],
                        ),
                      ),
                    ),
                    const SizedBox(height: 12),
                    // ── Voltage protection ──────────────────────────────
                    // Voltage thresholds (OV/UV/PL) are set once on the physical
                    // device (pump01) and apply to both pumps — same power meter.
                    if (!_isRelay2)
                      Card(
                        child: Padding(
                          padding: const EdgeInsets.all(16),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              const Text('Voltage Protection',
                                  style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                              const SizedBox(height: 12),
                              _field('Over Voltage',  _ovCtrl, 'V', _validateOv,
                                  hint: 'e.g. 480'),
                              _field('Under Voltage', _uvCtrl, 'V', _validateUv,
                                  hint: 'e.g. 360'),
                              _field('Phase Loss',    _plCtrl, 'V', _validatePositive,
                                  hint: 'e.g. 200'),
                            ],
                          ),
                        ),
                      )
                    else
                      Card(
                        color: Colors.orange.shade50,
                        child: const Padding(
                          padding: EdgeInsets.all(14),
                          child: Row(
                            children: [
                              Icon(Icons.info_outline, size: 18, color: Colors.orange),
                              SizedBox(width: 8),
                              Expanded(
                                child: Text(
                                  'Voltage protection (OV / UV / Phase Loss) is shared — '
                                  'configure it via Pump 1 settings.',
                                  style: TextStyle(fontSize: 12, color: Colors.orange),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    const SizedBox(height: 12),
                    // ── Dry run protection (optional) ───────────────────
                    Card(
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Row(
                              children: [
                                const Text('Dry Run Protection',
                                    style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                                const Spacer(),
                                Switch(
                                  value: _dryRunEnabled,
                                  onChanged: (v) => setState(() => _dryRunEnabled = v),
                                ),
                              ],
                            ),
                            const SizedBox(height: 12),
                            _field('Startup Delay', _startTCtrl, 's', _validateInt,
                                hint: 'e.g. 90'),
                            const Padding(
                              padding: EdgeInsets.only(bottom: 6),
                              child: Text(
                                'Dry run detection is skipped for this many seconds after relay turns ON '
                                '— covers soft-starter / star-delta delay.',
                                style: TextStyle(fontSize: 11, color: Colors.grey),
                              ),
                            ),
                            if (_dryRunEnabled) ...[
                              _field('Current Threshold', _dryICtrl, 'A', _validatePositive,
                                  hint: 'e.g. 3.0'),
                              _field('Trip Delay', _dryTCtrl, 's', _validateInt,
                                  hint: 'e.g. 8'),
                            ] else
                              const Padding(
                                padding: EdgeInsets.only(top: 8),
                                child: Text(
                                  'Disabled — pump runs without current monitoring.',
                                  style: TextStyle(fontSize: 12, color: Colors.grey),
                                ),
                              ),
                          ],
                        ),
                      ),
                    ),
                    const SizedBox(height: 12),
                    // ── Active on device ─────────────────────────────────
                    // Always shown. Values come from the device's last published
                    // status (persisted in Firebase). Shows "Waiting…" when the
                    // device has not published yet (new device / fresh Firebase).
                    Card(
                      color: Colors.blue.shade50,
                      child: Padding(
                        padding: const EdgeInsets.all(12),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text('Active on device',
                                style: TextStyle(fontWeight: FontWeight.bold,
                                    fontSize: 13, color: Colors.blueGrey)),
                            const SizedBox(height: 6),
                            if (_isRelay2
                                ? (_devDryEn2 == null)
                                : (_devOv == null))
                              const Text(
                                'Waiting for device status…',
                                style: TextStyle(fontSize: 12, color: Colors.grey),
                              )
                            else if (_isRelay2) ...[
                              // pump02 — relay2-specific cfg fields
                              if (_devHp2 != null && _devHp2! > 0)
                                _deviceRow('Rating', _devHp2 == 75 ? '7.5 HP' : '$_devHp2 HP'),
                              if (_devStartT2 != null)
                                _deviceRow('Startup delay', '$_devStartT2 s'),
                              _deviceRow('Dry Run', _devDryEn2 == 1 ? 'Enabled' : 'Disabled'),
                              if (_devDryEn2 == 1) ...[
                                _deviceRow('Dry I', '${_devDryI2?.toStringAsFixed(1)} A'),
                                _deviceRow('Dry T', '$_devDryT2 s'),
                              ],
                            ] else ...[
                              // pump01 — full cfg fields
                              if (_devHp != null && _devHp! > 0)
                                _deviceRow('Rating', _devHp == 75 ? '7.5 HP' : '$_devHp HP'),
                              _deviceRow('OV',    '${_devOv?.toStringAsFixed(0)} V'),
                              _deviceRow('UV',    '${_devUv?.toStringAsFixed(0)} V'),
                              _deviceRow('PL',    '${_devPl?.toStringAsFixed(0)} V'),
                              if (_devStartT != null)
                                _deviceRow('Startup delay', '$_devStartT s'),
                              if (_devDryEn != null)
                                _deviceRow('Dry Run', _devDryEn == 1 ? 'Enabled' : 'Disabled'),
                              if (_devDryEn == 1) ...[
                                _deviceRow('Dry I', '${_devDryI?.toStringAsFixed(1)} A'),
                                _deviceRow('Dry T', '$_devDryT s'),
                              ],
                            ],
                          ],
                        ),
                      ),
                    ),
                    const SizedBox(height: 16),
                    ElevatedButton(
                      onPressed: _saving ? null : _save,
                      style: ElevatedButton.styleFrom(
                        padding: const EdgeInsets.symmetric(vertical: 14),
                      ),
                      child: _saving
                          ? const SizedBox(height: 20, width: 20,
                              child: CircularProgressIndicator(strokeWidth: 2))
                          : const Text('Save Settings',
                              style: TextStyle(fontWeight: FontWeight.bold)),
                    ),
                    const SizedBox(height: 8),
                    const Text(
                      'Settings are saved to Firebase and forwarded to the device via MQTT.\n'
                      'The device applies them immediately and echoes back the active values above.',
                      style: TextStyle(fontSize: 11, color: Colors.grey),
                      textAlign: TextAlign.center,
                    ),
                  ],
                ),
              ),
            ),
    );
  }
}

// ─── Logs page ────────────────────────────────────────────────────────────────
class LogsPage extends StatefulWidget {
  final List<String> pumpIds;
  const LogsPage({super.key, required this.pumpIds});
  @override
  State<LogsPage> createState() => _LogsPageState();
}

class _LogsPageState extends State<LogsPage> {
  final db = FirebaseDatabase.instance;
  late String _pumpId;
  List<Map<String, dynamic>> _logs = [];
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _pumpId = widget.pumpIds.isNotEmpty ? widget.pumpIds.first : 'pump01';
    _loadLogs();
  }

  Future<void> _loadLogs() async {
    setState(() => _loading = true);
    final snap = await db
        .ref('pumps/$_pumpId/logs')
        .orderByKey()
        .limitToLast(60)
        .get();
    final data = snap.value as Map?;
    if (data == null) {
      setState(() { _logs = []; _loading = false; });
      return;
    }
    final entries = data.entries.toList()
      ..sort((a, b) => b.key.toString().compareTo(a.key.toString()));
    setState(() {
      _logs = entries.map((e) =>
          Map<String, dynamic>.from(e.value as Map)).toList();
      _loading = false;
    });
  }

  void _onPumpChanged(String pump) {
    setState(() => _pumpId = pump);
    _loadLogs();
  }

  String _formatRunTime(int s) {
    if (s < 60)   return '${s}s';
    if (s < 3600) return '${s ~/ 60}m ${s % 60}s';
    return '${s ~/ 3600}h ${(s % 3600) ~/ 60}m';
  }

  String _formatTs(int tsMs) {
    final dt = DateTime.fromMillisecondsSinceEpoch(tsMs).toLocal();
    final d = '${dt.day.toString().padLeft(2,'0')}/'
              '${dt.month.toString().padLeft(2,'0')}';
    final t = '${dt.hour.toString().padLeft(2,'0')}:'
              '${dt.minute.toString().padLeft(2,'0')}';
    return '$d  $t';
  }

  String _reasonLabel(String r) {
    const m = {
      'manual':       'Manual',
      'sched':        'Schedule',
      'rot':          'Rotation',
      'overvoltage':  'Overvoltage',
      'undervoltage': 'Undervoltage',
      'phase_loss':   'Phase Loss',
      'dry_run':      'Dry Run',
      'lora':         'LoRa',
    };
    return m[r] ?? r;
  }

  bool _isProtectionTrip(String reason) =>
      const {'overvoltage','undervoltage','phase_loss','dry_run'}.contains(reason);

  IconData _icon(String event, String reason) {
    if (event == 'on') return Icons.play_circle_outline;
    if (_isProtectionTrip(reason)) {
      return reason == 'dry_run' ? Icons.warning_amber : Icons.bolt;
    }
    if (reason == 'sched') return Icons.schedule;
    if (reason == 'rot')   return Icons.swap_horiz;
    return Icons.stop_circle_outlined;
  }

  Color _color(String event, String reason) {
    if (event == 'on') return Colors.green.shade700;
    if (_isProtectionTrip(reason)) return Colors.red.shade700;
    return Colors.grey.shade700;
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Pump Logs'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          IconButton(icon: const Icon(Icons.refresh), onPressed: _loadLogs),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
            child: SegmentedButton<String>(
              segments: [
                ...widget.pumpIds.map((id) {
                  final num = id.replaceAll('pump', '');
                  return ButtonSegment<String>(value: id, label: Text('Pump $num'));
                }),
              ],
              selected: {_pumpId},
              onSelectionChanged: (s) => _onPumpChanged(s.first),
            ),
          ),
          Expanded(
            child: _loading
                ? const Center(child: CircularProgressIndicator())
                : _logs.isEmpty
                    ? const Center(
                        child: Text('No logs yet.',
                            style: TextStyle(color: Colors.grey)))
                    : ListView.separated(
                        itemCount: _logs.length,
                        separatorBuilder: (_, __) =>
                            const Divider(height: 1, indent: 56),
                        itemBuilder: (ctx, i) {
                          final log    = _logs[i];
                          final event  = log['event']  as String? ?? '';
                          final reason = log['reason'] as String? ?? '';
                          final runS   = (log['run_s'] as num?)?.toInt() ?? 0;
                          final ts     = (log['ts']    as num?)?.toInt() ?? 0;
                          final c = _color(event, reason);
                          return ListTile(
                            dense: true,
                            leading: CircleAvatar(
                              radius: 18,
                              backgroundColor: c.withValues(alpha: 0.12),
                              child: Icon(_icon(event, reason), color: c, size: 20),
                            ),
                            title: Text(
                              '${event == 'on' ? 'ON' : 'OFF'}  ·  ${_reasonLabel(reason)}',
                              style: TextStyle(
                                  fontWeight: FontWeight.w600,
                                  color: c,
                                  fontSize: 13),
                            ),
                            subtitle: event == 'off' && runS > 0
                                ? Text('Run time: ${_formatRunTime(runS)}',
                                    style: const TextStyle(fontSize: 12))
                                : null,
                            trailing: ts > 0
                                ? Text(_formatTs(ts),
                                    style: const TextStyle(
                                        fontSize: 11, color: Colors.grey))
                                : null,
                          );
                        },
                      ),
          ),
        ],
      ),
    );
  }
}

// ─── Relay toggle button ──────────────────────────────────────────────────────
class _RelayButton extends StatelessWidget {
  final String label;
  final bool isOn;
  final bool disabled;
  final ValueChanged<bool> onToggle;
  const _RelayButton(
      {required this.label, required this.isOn, this.disabled = false, required this.onToggle});

  @override
  Widget build(BuildContext context) {
    return ElevatedButton(
      style: ElevatedButton.styleFrom(
        backgroundColor: disabled
            ? Colors.grey.shade200
            : isOn ? Colors.green : Colors.grey.shade300,
        foregroundColor: disabled ? Colors.grey : isOn ? Colors.white : Colors.black87,
        padding: const EdgeInsets.symmetric(vertical: 14),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      ),
      onPressed: disabled ? null : () => onToggle(!isOn),
      child: Text('$label\n${isOn ? "ON" : "OFF"}',
          textAlign: TextAlign.center,
          style: const TextStyle(fontWeight: FontWeight.bold)),
    );
  }
}
