using System;
using System.Threading;
using System.Windows.Forms;

namespace DokanSSHFS
{
    class DokanSSHFS
    {
        public static bool DokanDebug = false;
        public static bool SSHDebug = false;
        public static ushort DokanThread = 0;
        public static bool UseOffline = true;

        [STAThread]
        static void Main()
        {
            //ConsoleWin.Open();

            string[] args = System.Environment.GetCommandLineArgs();
            foreach (string arg in args)
            {
                if (arg == "-sd")
                {
                    SSHDebug = true;
                }
                if (arg == "-dd")
                {
                    DokanDebug = true;
                }
                if (arg.Length >= 3 &&
                    arg[0] == '-' &&
                    arg[1] == 't')
                {
                    DokanThread = ushort.Parse(arg.Substring(2));
                }
                if (arg == "-no")
                {
                    UseOffline = false;
                }
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SettingForm());

            /*
            ParseArgs parser = new ParseArgs();
            parser.parse(args);

            if (!parser.CheckParam())
            {
                parser.help();
                return;
            }

            DokanOptions opt = new DokanOptions();

            opt.DebugMode = parser.debug;
            opt.DriveLetter = parser.drive;
            opt.ThreadCount = parser.threads;

            SSHFS sshfs = new SSHFS(parser.user,
                parser.host, parser.port, parser.identity, parser.root, parser.debug);

            if (sshfs.SSHConnect())
            {
                DokanNet.DokanNet.DokanMain(opt, sshfs);
            }
            else
            {
                Console.Error.WriteLine("failed to connect");
            }

            Console.Error.WriteLine("sshfs exit");
             */
        }
    }
}
