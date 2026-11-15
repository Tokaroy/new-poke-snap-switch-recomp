# writes host/routes/hdrev_<course>.txt: nav, start the course, ride to the end (frame pairs come from NVN_FRAME_DUMP_AT)
NAV = {  # D-pad path on the Lental map, then course-panel keys
 'florio_a': ([], []), 'florio_b': ([], ['Down']),
 'beach': (['Right', 'Right'], []), 'jungle': (['Up'], []), 'river': (['Right', 'Up'], []),
 'volcano': (['Right'] * 4 + ['Up'], []), 'cave': (['Right'] * 5 + ['Up'], []), 'snow': (['Right'] * 5 + ['Up', 'Up'], []),
 'ruins': (['Right'] * 5, []),
}
for c, (path, panel) in NAV.items():
    est = 6139 + 360 * len(path) + 150 * len(panel)
    L = ['# fixture: unlock_all', '# HD texture review (worker hdpoke, 2026-11-14): ride %s to the end, no shutter, no camera input' % c,
         '# run: scripts/hd/review/run_hd.ps1 (SNAP_MODS_AB=1, SNAP_NEO_SPEED=3, NVN_FRAME_DUMP_AT pairs N,N+1 from the ride start, about present %d)' % est,
         'wait ui=title timeout=200', 'repeat @f60 @f30000 40 4 A', 'wait ui=map_select:StageSelect timeout=300']
    f = 120
    for k in path: L.append('@f%d 6 %s' % (f, k)); f += 360
    L.append('@f%d 6 A' % (f + 180))
    L.append('wait ui=map_select:CourseSelect timeout=60')
    f = 180
    for k in panel: L.append('@f%d 6 %s' % (f, k)); f += 150
    L.append('@f%d 6 A' % f)
    L += ['wait ride timeout=300', 'end @f9000']
    open('host/routes/hdrev_%s.txt' % c, 'w', newline='\n').write('\n'.join(L) + '\n')
    print(c, est)
