// lib/logs_page.dart
// Pump logs — Events tab (history list) + Charts tab (3 charts)
// Charts data sources:
//   pumps/{pumpId}/voltage_log/{key}: {ts, v1, v2, v3, current}  (15-min snapshots)
//   pumps/{pumpId}/logs/{key}:        {event, reason, run_s, ts}

import 'package:flutter/material.dart';
import 'package:firebase_database/firebase_database.dart';
import 'package:fl_chart/fl_chart.dart';

// ─── Voltage log entry ────────────────────────────────────────────────────────
class _VEntry {
  final int    ts;
  final double v1;
  final double v2;
  final double v3;
  final double current;
  final double kw;      // Total active power from meter (kW)

  const _VEntry({
    required this.ts,
    required this.v1,
    required this.v2,
    required this.v3,
    required this.current,
    required this.kw,
  });

  factory _VEntry.fromMap(Map<Object?, Object?> m) => _VEntry(
        ts:      (m['ts']      as num?)?.toInt()    ?? 0,
        v1:      (m['v1']      as num?)?.toDouble() ?? 0,
        v2:      (m['v2']      as num?)?.toDouble() ?? 0,
        v3:      (m['v3']      as num?)?.toDouble() ?? 0,
        current: (m['current'] as num?)?.toDouble() ?? 0,
        kw:      (m['kw']      as num?)?.toDouble() ?? 0,
      );
}

// ─── Page ─────────────────────────────────────────────────────────────────────
class LogsPage extends StatefulWidget {
  final List<String> pumpIds;
  const LogsPage({super.key, required this.pumpIds});

  @override
  State<LogsPage> createState() => _LogsPageState();
}

class _LogsPageState extends State<LogsPage>
    with SingleTickerProviderStateMixin {
  final _db = FirebaseDatabase.instance;
  late TabController _tab;
  late String _pumpId;

  // ── Events state ────────────────────────────────────────────────────────────
  List<Map<String, dynamic>> _logs = [];
  bool _loadingLogs = true;

  // ── Charts state ────────────────────────────────────────────────────────────
  List<_VEntry> _vlog = [];
  int _windowStartMs  = 0; // midnight of the selected day-window (ms epoch)
  int _dayOffset      = 0; // 0 = today, 1 = yesterday, 2 = 2 days ago …
  // per-pump runtime: pumpId → dayLabel(dd/MM) → totalSeconds (from event logs)
  Map<String, Map<String, int>> _pumpRuntime = {};
  // all voltage_log entries from the last 7-day fetch — used for prev-window peak/kWh
  List<_VEntry> _allVlog = [];
  bool _loadingCharts = true;

  // ── Lifecycle ───────────────────────────────────────────────────────────────
  @override
  void initState() {
    super.initState();
    _tab    = TabController(length: 2, vsync: this);
    _pumpId = widget.pumpIds.isNotEmpty ? widget.pumpIds.first : 'pump01';
    _loadLogs();
    _loadCharts();
  }

  @override
  void dispose() {
    _tab.dispose();
    super.dispose();
  }

  // ── Data loading ─────────────────────────────────────────────────────────────

  Future<void> _loadLogs() async {
    if (!mounted) return;
    setState(() => _loadingLogs = true);
    final snap = await _db
        .ref('pumps/$_pumpId/logs')
        .orderByKey()
        .limitToLast(60)
        .get();
    if (!mounted) return;
    final data = snap.value as Map?;
    if (data == null) {
      setState(() { _logs = []; _loadingLogs = false; });
      return;
    }
    final entries = data.entries.toList()
      ..sort((a, b) => b.key.toString().compareTo(a.key.toString()));
    setState(() {
      _logs = entries
          .map((e) => Map<String, dynamic>.from(e.value as Map))
          .toList();
      _loadingLogs = false;
    });
  }

  Future<void> _loadCharts() async {
    if (!mounted) return;
    setState(() => _loadingCharts = true);
    await Future.wait([_loadVoltageLog(), _loadRuntime(), _loadPumpRuntime()]);
    if (mounted) setState(() => _loadingCharts = false);
  }

  Future<void> _loadVoltageLog() async {
    // Base = most recent 6AM that has already passed; apply day offset
    final now      = DateTime.now();
    final winStart = DateTime(now.year, now.month, now.day)
        .subtract(Duration(days: _dayOffset));
    _windowStartMs = winStart.millisecondsSinceEpoch;
    final winEndMs  = _windowStartMs + 24 * 3600 * 1000;

    final chartPump = widget.pumpIds.isNotEmpty ? widget.pumpIds.first : 'pump01';
    // limitToLast(500) covers up to 5 days of 15-min slots; filter client-side
    final snap = await _db
        .ref('pumps/$chartPump/voltage_log')
        .orderByKey()
        .limitToLast(500)
        .get();
    final data = snap.value as Map?;
    if (data == null) { _vlog = []; return; }
    _vlog = data.entries
        .map((e) => _VEntry.fromMap(e.value as Map<Object?, Object?>))
        .where((e) => e.ts >= _windowStartMs && e.ts < winEndMs)
        .toList()
      ..sort((a, b) => a.ts.compareTo(b.ts));
  }

  Future<void> _loadRuntime() async {
    // Fetch voltage_log for the peak-kW / kWh helpers (_windowPeakKw, _windowKwhTotal).
    final deviceId = widget.pumpIds.isNotEmpty ? widget.pumpIds.first : 'pump01';
    final snap = await _db
        .ref('pumps/$deviceId/voltage_log')
        .orderByKey()
        .limitToLast(700) // ~7 days × 96 slots/day
        .get();
    final data = snap.value as Map?;
    final List<_VEntry> all = [];
    if (data != null) {
      for (final e in data.entries) {
        final entry = _VEntry.fromMap(e.value as Map<Object?, Object?>);
        if (entry.ts == 0) continue;
        all.add(entry); // keep all for peak/kWh helpers
      }
    }
    _allVlog = all;
  }

  Future<void> _loadPumpRuntime() async {
    // Read each pump's event log and sum run_s per calendar day (from OFF events).
    // This gives per-pump breakdown needed for the stacked bar chart.
    final cutoffMs = DateTime.now()
        .subtract(const Duration(days: 8))
        .millisecondsSinceEpoch;
    final Map<String, Map<String, int>> result = {};
    await Future.wait(widget.pumpIds.map((pumpId) async {
      final snap = await _db
          .ref('pumps/$pumpId/logs')
          .orderByKey()
          .limitToLast(500)
          .get();
      final data = snap.value as Map?;
      if (data == null) { result[pumpId] = {}; return; }
      final Map<String, int> dayMap = {};
      for (final e in data.entries) {
        final entry = Map<String, dynamic>.from(e.value as Map);
        final ev   = entry['event'] as String? ?? '';
        final ts   = (entry['ts']   as num?)?.toInt() ?? 0;
        final runS = (entry['run_s'] as num?)?.toInt() ?? 0;
        if (ev != 'off' || ts < cutoffMs || runS <= 0) continue;
        final dt    = DateTime.fromMillisecondsSinceEpoch(ts).toLocal();
        final label = '${dt.day.toString().padLeft(2, '0')}/'
                      '${dt.month.toString().padLeft(2, '0')}';
        dayMap[label] = (dayMap[label] ?? 0) + runS;
      }
      result[pumpId] = dayMap;
    }));
    _pumpRuntime = result;
  }

  void _onPumpChanged(String pump) {
    setState(() => _pumpId = pump);
    _loadLogs();
  }

  Future<void> _changeDay(int offset) async {
    if (!mounted || _dayOffset == offset) return;
    setState(() { _dayOffset = offset; _loadingCharts = true; });
    await _loadVoltageLog();
    if (mounted) setState(() => _loadingCharts = false);
  }

  void _refresh() {
    _loadLogs();
    _loadCharts();
  }

  // ── Time-axis helpers ────────────────────────────────────────────────────────

  // X value = minutes elapsed since the 6 AM window start
  double _xFor(_VEntry e) =>
      (e.ts - _windowStartMs) / 60000.0;

  // Label for a given X (minutes since 6AM) → "HH:MM"
  Widget _timeLabel(double v, TitleMeta _) {
    final totalMin = v.round() % (24 * 60);
    final h = totalMin ~/ 60;
    final m = totalMin % 60;
    return Padding(
      padding: const EdgeInsets.only(top: 4),
      child: Text(
        '${h.toString().padLeft(2,'0')}:${m.toString().padLeft(2,'0')}',
        style: const TextStyle(fontSize: 8),
      ),
    );
  }

  // Actual wall-clock time string from X value
  String _xToTime(double x) {
    final actualMs = _windowStartMs + (x * 60000).round();
    final dt = DateTime.fromMillisecondsSinceEpoch(actualMs).toLocal();
    return '${dt.day.toString().padLeft(2,'0')}/${dt.month.toString().padLeft(2,'0')}'
           ' ${dt.hour.toString().padLeft(2,'0')}:${dt.minute.toString().padLeft(2,'0')}';
  }

  // ── Events helpers ──────────────────────────────────────────────────────────

  String _formatRunTime(int s) {
    if (s < 60)   return '${s}s';
    if (s < 3600) return '${s ~/ 60}m ${s % 60}s';
    return '${s ~/ 3600}h ${(s % 3600) ~/ 60}m';
  }

  String _formatTs(int tsMs) {
    final dt = DateTime.fromMillisecondsSinceEpoch(tsMs).toLocal();
    return '${dt.day.toString().padLeft(2,'0')}/'
           '${dt.month.toString().padLeft(2,'0')}'
           '  ${dt.hour.toString().padLeft(2,'0')}:'
           '${dt.minute.toString().padLeft(2,'0')}';
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

  bool _isTrip(String r) =>
      const {'overvoltage','undervoltage','phase_loss','dry_run'}.contains(r);

  IconData _icon(String event, String reason) {
    if (event == 'on') return Icons.play_circle_outline;
    if (_isTrip(reason)) return reason == 'dry_run' ? Icons.warning_amber : Icons.bolt;
    if (reason == 'sched') return Icons.schedule;
    if (reason == 'rot')   return Icons.swap_horiz;
    return Icons.stop_circle_outlined;
  }

  Color _color(String event, String reason) {
    if (event == 'on') return Colors.green.shade700;
    if (_isTrip(reason)) return Colors.red.shade700;
    return Colors.grey.shade700;
  }

  // ── Chart 1: Current (A) ────────────────────────────────────────────────────

  Widget _buildCurrentChart() {
    if (_vlog.isEmpty) return _emptyChart('No data for this 6AM–6AM window');

    final spots = _vlog.map((e) => FlSpot(_xFor(e), e.current)).toList();

    final allA = _vlog.map((e) => e.current).where((v) => v > 0).toList();
    const minA = 0.0;
    final maxA = allA.isEmpty ? 20.0 : allA.reduce((a, b) => a > b ? a : b) + 2;

    return _ChartCard(
      title: 'Current (A)',
      child: LineChart(LineChartData(
        minX: 0,
        maxX: 1440,
        minY: minA,
        maxY: maxA,
        gridData: FlGridData(
          show: true,
          drawVerticalLine: true,
          verticalInterval: 360, // every 6 h
          getDrawingVerticalLine: (_) =>
              FlLine(color: Colors.grey.shade200, strokeWidth: 1),
          getDrawingHorizontalLine: (_) =>
              FlLine(color: Colors.grey.shade200, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        lineBarsData: [
          LineChartBarData(
            spots: spots,
            color: Colors.amber.shade700,
            isCurved: true,
            barWidth: 2,
            dotData: const FlDotData(show: false),
            belowBarData: BarAreaData(
              show: true,
              color: Colors.amber.shade700.withValues(alpha: 0.12),
            ),
          ),
        ],
        titlesData: _timeTitles(
          leftWidget: (v, _) =>
              Text(v.toStringAsFixed(0), style: const TextStyle(fontSize: 9)),
        ),
        lineTouchData: LineTouchData(
          touchTooltipData: LineTouchTooltipData(
            getTooltipColor: (_) => Colors.black87,
            getTooltipItems: (spots) => spots.map((s) => LineTooltipItem(
              '${_xToTime(s.x)}\n${s.y.toStringAsFixed(1)} A',
              const TextStyle(color: Colors.amber, fontSize: 11),
            )).toList(),
          ),
        ),
      )),
    );
  }

  // ── Chart 2: 3-Phase Voltage ─────────────────────────────────────────────────

  Widget _buildThreePhaseChart() {
    if (_vlog.isEmpty) return _emptyChart('No data for this 6AM–6AM window');

    final phaseColors = [
      Colors.red.shade500,
      Colors.green.shade600,
      Colors.blue.shade500,
    ];

    List<FlSpot> phaseSpots(double Function(_VEntry) val) =>
        _vlog.map((e) => FlSpot(_xFor(e), val(e))).toList();

    final allV = _vlog
        .expand((e) => [e.v1, e.v2, e.v3])
        .where((v) => v > 0)
        .toList();
    final minV = allV.isEmpty ? 180.0 : allV.reduce((a, b) => a < b ? a : b) - 10;
    final maxV = allV.isEmpty ? 260.0 : allV.reduce((a, b) => a > b ? a : b) + 10;

    return _ChartCard(
      title: '3-Phase Voltage (V)',
      legend: Row(children: [
        _LegendDot(color: phaseColors[0], label: 'L1'),
        const SizedBox(width: 10),
        _LegendDot(color: phaseColors[1], label: 'L2'),
        const SizedBox(width: 10),
        _LegendDot(color: phaseColors[2], label: 'L3'),
      ]),
      child: LineChart(LineChartData(
        minX: 0,
        maxX: 1440,
        minY: minV,
        maxY: maxV,
        gridData: FlGridData(
          show: true,
          drawVerticalLine: true,
          verticalInterval: 360,
          getDrawingVerticalLine: (_) =>
              FlLine(color: Colors.grey.shade200, strokeWidth: 1),
          getDrawingHorizontalLine: (_) =>
              FlLine(color: Colors.grey.shade200, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        lineBarsData: [
          _phaseLine(phaseSpots((e) => e.v1), phaseColors[0]),
          _phaseLine(phaseSpots((e) => e.v2), phaseColors[1]),
          _phaseLine(phaseSpots((e) => e.v3), phaseColors[2]),
        ],
        titlesData: _timeTitles(
          leftWidget: (v, _) =>
              Text(v.toStringAsFixed(0), style: const TextStyle(fontSize: 9)),
          leftReserved: 40,
        ),
        lineTouchData: LineTouchData(
          touchTooltipData: LineTouchTooltipData(
            getTooltipColor: (_) => Colors.black87,
            getTooltipItems: (spots) {
              const labels = ['L1', 'L2', 'L3'];
              return spots.map((s) {
                final phase = s.barIndex < 3 ? labels[s.barIndex] : '?';
                final c     = phaseColors[s.barIndex < 3 ? s.barIndex : 0];
                return LineTooltipItem(
                  '$phase: ${s.y.toStringAsFixed(0)} V\n${_xToTime(s.x)}',
                  TextStyle(color: c, fontSize: 11),
                );
              }).toList();
            },
          ),
        ),
      )),
    );
  }

  LineChartBarData _phaseLine(List<FlSpot> spots, Color color) =>
      LineChartBarData(
        spots: spots,
        color: color,
        isCurved: true,
        barWidth: 2,
        dotData: const FlDotData(show: false),
      );

  // ── Chart 4: Daily Runtime — stacked bar (one segment per pump) ────────────

  Widget _buildDailyRuntimeChart() {
    // Collect all day labels across all pumps
    final allDays = <String>{};
    for (final pMap in _pumpRuntime.values) allDays.addAll(pMap.keys);
    final sortedDays = allDays.toList()..sort();
    final last7 = sortedDays.length > 7
        ? sortedDays.sublist(sortedDays.length - 7)
        : sortedDays;
    if (last7.isEmpty) return _emptyChart('No runtime data');

    const segColors = [Color(0xFF26A69A), Color(0xFFFF7043)]; // teal=pump01, orange=pump02
    final pumpIds = widget.pumpIds;

    double maxY = 0;
    final groups = last7.asMap().entries.map((e) {
      final day = e.value;
      double base = 0;
      final stack = <BarChartRodStackItem>[];
      for (int i = 0; i < pumpIds.length; i++) {
        final secs  = (_pumpRuntime[pumpIds[i]]?[day] ?? 0).toDouble();
        final hours = secs / 3600;
        if (hours > 0) {
          stack.add(BarChartRodStackItem(base, base + hours,
              segColors[i % segColors.length]));
          base += hours;
        }
      }
      if (base > maxY) maxY = base;
      return BarChartGroupData(
        x: e.key,
        barRods: [
          BarChartRodData(
            toY: base,
            width: 20,
            rodStackItems: stack,
            borderRadius: BorderRadius.circular(3),
            color: Colors.transparent,
          ),
        ],
      );
    }).toList();

    return _ChartCard(
      title: 'Daily Runtime (hours)',
      legend: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (int i = 0; i < pumpIds.length && i < segColors.length; i++) ...[
            if (i > 0) const SizedBox(width: 12),
            Container(
              width: 12, height: 12,
              decoration: BoxDecoration(
                color: segColors[i],
                borderRadius: BorderRadius.circular(2),
              ),
            ),
            const SizedBox(width: 4),
            Text(
              'Pump ${pumpIds[i].replaceAll('pump', '')}',
              style: const TextStyle(fontSize: 11),
            ),
          ],
        ],
      ),
      child: BarChart(BarChartData(
        barGroups: groups,
        groupsSpace: 12,
        maxY: maxY > 0 ? maxY + 1 : 10,
        gridData: FlGridData(show: true, drawVerticalLine: false),
        borderData: FlBorderData(show: false),
        titlesData: FlTitlesData(
          leftTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: 36,
              getTitlesWidget: (v, _) =>
                  Text('${v.toStringAsFixed(0)}h',
                      style: const TextStyle(fontSize: 9)),
            ),
          ),
          bottomTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: 22,
              getTitlesWidget: (v, _) {
                final idx = v.toInt();
                if (idx < 0 || idx >= last7.length) return const SizedBox.shrink();
                return Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: Text(last7[idx], style: const TextStyle(fontSize: 9)),
                );
              },
            ),
          ),
          rightTitles: AxisTitles(sideTitles: SideTitles(showTitles: false)),
          topTitles:   AxisTitles(sideTitles: SideTitles(showTitles: false)),
        ),
        barTouchData: BarTouchData(
          touchTooltipData: BarTouchTooltipData(
            getTooltipColor: (_) => Colors.black87,
            getTooltipItem: (group, _, rod, __) {
              final day = last7[group.x];
              final buf = StringBuffer('$day\n');
              for (int i = 0; i < pumpIds.length; i++) {
                final secs = _pumpRuntime[pumpIds[i]]?[day] ?? 0;
                final h    = secs ~/ 3600;
                final m    = (secs % 3600) ~/ 60;
                buf.write('Pump ${pumpIds[i].replaceAll("pump", "")}: '
                    '${h}h ${m.toString().padLeft(2, "0")}m\n');
              }
              return BarTooltipItem(
                buf.toString().trimRight(),
                const TextStyle(color: Colors.white, fontSize: 11),
              );
            },
          ),
        ),
      )),
    );
  }

  // ── Helpers: per-window kW peak and kWh total ───────────────────────────

  // Max kW reading within a 24-h window (any entry, not just running)
  double _windowPeakKw(int winStartMs) {
    final winEndMs = winStartMs + 24 * 3600 * 1000;
    double peak = 0;
    for (final e in _allVlog) {
      if (e.ts >= winStartMs && e.ts < winEndMs && e.kw > peak) peak = e.kw;
    }
    return peak;
  }

  // Total kWh for a 24-h window: kW × 0.25 h per 15-min slot
  double _windowKwhTotal(int winStartMs) {
    final winEndMs = winStartMs + 24 * 3600 * 1000;
    double total = 0;
    for (final e in _allVlog) {
      if (e.ts >= winStartMs && e.ts < winEndMs) total += e.kw * 0.25;
    }
    return total;
  }

  // ── Chart 3: Power consumption (kW line, time axis) ──────────────────────

  Widget _buildPowerChart() {
    if (_vlog.isEmpty) return _emptyChart('No data for this 6AM–6AM window');

    final spots  = _vlog.map((e) => FlSpot(_xFor(e), e.kw)).toList();
    final allKw  = _vlog.map((e) => e.kw).where((v) => v > 0).toList();
    final maxKw  = allKw.isEmpty ? 5.0 : allKw.reduce((a, b) => a > b ? a : b) + 0.5;

    final prevPeak = _windowPeakKw(_windowStartMs - 24 * 3600 * 1000);
    final totalKwh = _windowKwhTotal(_windowStartMs);

    return _ChartCard(
      title: 'Power Consumption',
      legend: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.bolt, size: 14, color: Colors.orange),
          const SizedBox(width: 4),
          Text(
            '${totalKwh.toStringAsFixed(2)} kWh',
            style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 13),
          ),
        ],
      ),
      child: LineChart(LineChartData(
        minX: 0,
        maxX: 1440,
        minY: 0,
        maxY: maxKw,
        extraLinesData: prevPeak > 0
            ? ExtraLinesData(horizontalLines: [
                HorizontalLine(
                  y: prevPeak,
                  color: Colors.grey.shade400,
                  strokeWidth: 1.5,
                  dashArray: [6, 4],
                  label: HorizontalLineLabel(
                    show: true,
                    alignment: Alignment.topRight,
                    padding: const EdgeInsets.only(right: 6, bottom: 2),
                    labelResolver: (_) => "Yesterday's high",
                    style: TextStyle(
                      fontSize: 9,
                      color: Colors.grey.shade600,
                      fontStyle: FontStyle.italic,
                    ),
                  ),
                ),
              ])
            : null,
        gridData: FlGridData(
          show: true,
          drawVerticalLine: true,
          verticalInterval: 360,
          getDrawingVerticalLine: (_) =>
              FlLine(color: Colors.grey.shade200, strokeWidth: 1),
          getDrawingHorizontalLine: (_) =>
              FlLine(color: Colors.grey.shade200, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        lineBarsData: [
          LineChartBarData(
            spots: spots,
            color: Colors.green.shade600,
            isCurved: true,
            barWidth: 2,
            dotData: const FlDotData(show: false),
            belowBarData: BarAreaData(
              show: true,
              color: Colors.green.shade600.withValues(alpha: 0.15),
            ),
          ),
        ],
        titlesData: _timeTitles(
          leftWidget: (v, _) =>
              Text(v.toStringAsFixed(1), style: const TextStyle(fontSize: 9)),
        ),
        lineTouchData: LineTouchData(
          touchTooltipData: LineTouchTooltipData(
            getTooltipColor: (_) => Colors.black87,
            getTooltipItems: (spots) => spots.map((s) => LineTooltipItem(
              '${_xToTime(s.x)}\n${s.y.toStringAsFixed(2)} kW',
              TextStyle(color: Colors.green.shade400, fontSize: 11),
            )).toList(),
          ),
        ),
      )),
    );
  }

  // ── Shared axis / layout helpers ─────────────────────────────────────────────

  // Bottom axis: 6-hour ticks (0, 360, 720, 1080, 1440 minutes since 6AM)
  // Left axis: custom widget; right + top hidden
  FlTitlesData _timeTitles({
    required Widget Function(double, TitleMeta) leftWidget,
    double leftReserved = 36,
  }) {
    return FlTitlesData(
      leftTitles: AxisTitles(
        sideTitles: SideTitles(
          showTitles: true,
          reservedSize: leftReserved,
          getTitlesWidget: leftWidget,
        ),
      ),
      bottomTitles: AxisTitles(
        sideTitles: SideTitles(
          showTitles: true,
          reservedSize: 26,
          interval: 360, // every 6 hours
          getTitlesWidget: _timeLabel,
        ),
      ),
      rightTitles: AxisTitles(sideTitles: SideTitles(showTitles: false)),
      topTitles:   AxisTitles(sideTitles: SideTitles(showTitles: false)),
    );
  }

  Widget _emptyChart(String msg) => SizedBox(
    height: 180,
    child: Center(child: Text(msg, style: const TextStyle(color: Colors.grey))),
  );

  // ── Tab views ────────────────────────────────────────────────────────────────

  Widget _buildEventsTab() {
    return Column(children: [
      Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
        child: SegmentedButton<String>(
          segments: widget.pumpIds.map((id) {
            final num = id.replaceAll('pump', '');
            return ButtonSegment<String>(value: id, label: Text('Pump $num'));
          }).toList(),
          selected: {_pumpId},
          onSelectionChanged: (s) => _onPumpChanged(s.first),
        ),
      ),
      Expanded(child: _buildEventsList()),
    ]);
  }

  Widget _buildEventsList() {
    if (_loadingLogs) return const Center(child: CircularProgressIndicator());
    if (_logs.isEmpty) {
      return const Center(
          child: Text('No logs yet.', style: TextStyle(color: Colors.grey)));
    }
    return ListView.separated(
      itemCount: _logs.length,
      separatorBuilder: (_, __) => const Divider(height: 1, indent: 56),
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
                fontWeight: FontWeight.w600, color: c, fontSize: 13),
          ),
          subtitle: event == 'off' && runS > 0
              ? Text('Run time: ${_formatRunTime(runS)}',
                  style: const TextStyle(fontSize: 12))
              : null,
          trailing: ts > 0
              ? Text(_formatTs(ts),
                  style: const TextStyle(fontSize: 11, color: Colors.grey))
              : null,
        );
      },
    );
  }

  String _dayLabel(int offset) {
    if (offset == 0) return 'Today';
    if (offset == 1) return 'Yesterday';
    final day    = DateTime.now().subtract(Duration(days: offset));
    const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    return '${day.day} ${months[day.month - 1]} ${day.year}';
  }

  Widget _buildDaySelector() {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 6),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          // ← older day
          IconButton(
            icon: const Icon(Icons.chevron_left),
            tooltip: 'Previous day',
            onPressed: _dayOffset < 4 ? () => _changeDay(_dayOffset + 1) : null,
          ),
          // centre: calendar icon + label
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.calendar_today_outlined, size: 16, color: Colors.grey),
              const SizedBox(width: 6),
              Text(
                _dayLabel(_dayOffset),
                style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 15),
              ),
            ],
          ),
          // → newer day (disabled when already on today)
          IconButton(
            icon: const Icon(Icons.chevron_right),
            tooltip: 'Next day',
            onPressed: _dayOffset > 0 ? () => _changeDay(_dayOffset - 1) : null,
          ),
        ],
      ),
    );
  }

  Widget _buildChartsTab() {
    return Column(children: [
      _buildDaySelector(),
      Expanded(
        child: _loadingCharts
            ? const Center(child: CircularProgressIndicator())
            : SingleChildScrollView(
                padding: const EdgeInsets.fromLTRB(12, 4, 12, 12),
                child: Column(children: [
                  _buildCurrentChart(),
                  const SizedBox(height: 12),
                  _buildThreePhaseChart(),
                  const SizedBox(height: 12),
                  _buildPowerChart(),
                  const SizedBox(height: 12),
                  _buildDailyRuntimeChart(),
                  const SizedBox(height: 24),
                ]),
              ),
      ),
    ]);
  }

  // ── Build ────────────────────────────────────────────────────────────────────

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Pump Logs'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          IconButton(icon: const Icon(Icons.refresh), onPressed: _refresh),
        ],
        bottom: TabBar(
          controller: _tab,
          tabs: const [Tab(text: 'Charts'), Tab(text: 'Events')],
        ),
      ),
      body: TabBarView(
        controller: _tab,
        children: [_buildChartsTab(), _buildEventsTab()],
      ),
    );
  }
}

// ─── Chart card wrapper ───────────────────────────────────────────────────────
class _ChartCard extends StatelessWidget {
  final String  title;
  final Widget  child;
  final Widget? legend;

  const _ChartCard({required this.title, required this.child, this.legend});

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title,
                style: const TextStyle(
                    fontWeight: FontWeight.w600, fontSize: 13)),
            if (legend != null) ...[
              const SizedBox(height: 6),
              legend!,
            ],
            const SizedBox(height: 8),
            SizedBox(height: 180, child: child),
          ],
        ),
      ),
    );
  }
}

// ─── Legend dot ──────────────────────────────────────────────────────────────
class _LegendDot extends StatelessWidget {
  final Color  color;
  final String label;
  const _LegendDot({required this.color, required this.label});

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: 10, height: 10,
          decoration: BoxDecoration(color: color, shape: BoxShape.circle),
        ),
        const SizedBox(width: 4),
        Text(label, style: const TextStyle(fontSize: 11)),
      ],
    );
  }
}
