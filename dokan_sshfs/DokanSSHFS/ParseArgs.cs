using System;
using System.Collections.Generic;
using System.Text;

namespace DokanSSHFS
{
    class ParseArgs
    {
        public string host;
        public int port;
        public string user;
        public string identity;
        public char drive;
        public bool debug;
        public uint threads;
        public string root;

        public ParseArgs()
        {
            threads = 0;
            debug = false;
            port = 22;
            drive = 'n';
            root = "";
        }

        public void parse(string[] args)
        {
            for (int i = 0; i < args.Length; ++i)
            {
                if ((args[i][0] == '-' || args[i][0] == '/')
                    && args[i].Length > 1 // -‚ÌŒã‚É•¶Žš‚ª‚ ‚é
                    && i + i < args.Length) // ŽŸ‚Ì—v‘f‚ª‚ ‚é
                {
                    switch (args[i][1])
                    {
                        case 'h':
                            host = args[++i];
                            break;
                        case 'p':
                            port = int.Parse(args[++i]);
                            break;
                        case 'u':
                            user = args[++i];
                            break;
                        case 'i':
                            identity = args[++i];
                            break;
                        case 'd':
                            drive = args[++i][0];
                            break;
                        case 'r':
                            root = args[++i];
                            break;
                        case 't':
                            threads = uint.Parse(args[++i]);
                            break;
                    }
                }

                if (args[i] == "-x")
                    debug = true;

                if (args[i].Contains("@"))
                {
                    user = args[i].Substring(0, args[i].IndexOf('@'));

                    host = args[i].Substring(args[i].IndexOf('@') + 1);
                    if (host.Contains(":"))
                    {
                        root = host.Substring(host.IndexOf(':') + 1);
                        host = host.Substring(0, host.IndexOf(':'));
                    }
                }
                else
                {
                    drive = args[i][0];
                }
            }
        }

        public bool CheckParam()
        {
            if (host == null || port == 0 || user == null || identity == null || drive == '\0')
                return false;
            return true;
        }

        public void help()
        {
            Console.Error.WriteLine("SSHFS");
            Console.Error.WriteLine("  -d drive letter [default n]");
            Console.Error.WriteLine("  -h host name");
            Console.Error.WriteLine("  -u user name");
            Console.Error.WriteLine("  -p port [default 22]");
            Console.Error.WriteLine("  -i ssh private key");
            Console.Error.WriteLine("  -r path to remote dir [default /]");
            Console.Error.WriteLine("  user@host drive");
            Console.Error.WriteLine();
            Console.Error.WriteLine("Example");
            Console.Error.WriteLine("  " + Environment.GetCommandLineArgs()[0] + " -i C:\\cygwin\\home\\user\\.ssh\\id_rsa user@example.com:/ n");
            Console.Error.WriteLine("  " + Environment.GetCommandLineArgs()[0] + " -i C:\\cygwin\\home\\user\\.ssh\\id_rsa -u user -h example.com -r / -p 22 -d n");
        }
    }
}
