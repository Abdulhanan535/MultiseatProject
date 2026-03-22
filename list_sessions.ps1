Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class WtsTest {
    [DllImport("wtsapi32.dll", SetLastError = true)]
    public static extern bool WTSEnumerateSessions(IntPtr hServer, int Reserved, int Version, out IntPtr ppSessionInfo, out int pCount);
    [DllImport("wtsapi32.dll")]
    public static extern void WTSFreeMemory(IntPtr pMemory);
    [StructLayout(LayoutKind.Sequential)]
    public struct WTS_SESSION_INFO { 
        public int SessionId; 
        [MarshalAs(UnmanagedType.LPStr)] public string pWinStationName; 
        public int State; 
    }
    public static void List() {
        IntPtr pSessions; int count;
        if (WTSEnumerateSessions(IntPtr.Zero, 0, 1, out pSessions, out count)) {
            IntPtr current = pSessions;
            int size = Marshal.SizeOf(typeof(WTS_SESSION_INFO));
            for (int i = 0; i < count; i++) {
                WTS_SESSION_INFO si = (WTS_SESSION_INFO)Marshal.PtrToStructure(current, typeof(WTS_SESSION_INFO));
                string stateStr = "";
                switch(si.State) {
                    case 0: stateStr="Active"; break;
                    case 1: stateStr="Connected"; break;
                    case 4: stateStr="Disconnected"; break;
                    case 5: stateStr="Listen"; break;
                    default: stateStr=si.State.ToString(); break;
                }
                Console.WriteLine("Session " + si.SessionId + ": " + si.pWinStationName + " [" + stateStr + "]");
                current = (IntPtr)((long)current + size);
            }
            WTSFreeMemory(pSessions);
        }
    }
}
"@
[WtsTest]::List()
