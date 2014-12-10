using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace DokanSSHFS
{
    class ConsoleWin
    {
        [DllImport("kernel32")]
        static extern void AllocConsole();


        const uint GENERIC_WRITE = 0x40000000;
        const uint OPEN_EXISTING = 3;

        const int STD_INPUT_HANDLE = -10;
        const int STD_OUTPUT_HANDLE = -11;
        const int STD_ERROR_HANDLE = -12;


        [DllImport("kernel32")]
        static extern int CreateFile(
                string filename,
                uint desiredAccess,
                uint shareMode,
                uint attributes,   // really SecurityAttributes pointer
                uint creationDisposition,
                uint flagsAndAttributes,
                uint templateFile);

        [DllImport("kernel32")]
        static extern int SetStdHandle(
            int stdHandle,
            int handle);

        public static void Open()
        {
            AllocConsole();
            int handle = CreateFile("CONOUT$", GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);

            SetStdHandle(STD_OUTPUT_HANDLE, handle);
            SetStdHandle(STD_ERROR_HANDLE, handle);

        }
    }
}
