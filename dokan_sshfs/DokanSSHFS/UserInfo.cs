using System;
using System.Collections.Generic;
using System.Text;

namespace DokanSSHFS
{
    class DokanUserInfo : Tamir.SharpSsh.jsch.UserInfo, Tamir.SharpSsh.jsch.UIKeyboardInteractive
    {
        private string password_;
        private string passphrase_;

        public DokanUserInfo(string password, string passphrase)
        {
            password_ = password;
            passphrase_ = passphrase;
        }

        #region UserInfo ÉÅÉìÉo

        public string getPassphrase()
        {
            return passphrase_;
        }

        public string getPassword()
        {
            return password_;
        }

        public bool promptPassphrase(string message)
        {
            System.Diagnostics.Debug.WriteLine(message);
            return false;
        }

        public bool promptPassword(string message)
        {
            System.Diagnostics.Debug.WriteLine(message);
            return false;
        }

        public bool promptYesNo(string message)
        {
            System.Diagnostics.Debug.WriteLine(message);
            return true;
        }

        public void showMessage(string message)
        {
            System.Windows.Forms.MessageBox.Show(message);
        }

        #endregion

        #region UIKeyboardInteractive ÉÅÉìÉo

        public string[] promptKeyboardInteractive(string destination, string name, string instruction, string[] prompt, bool[] echo)
        {
            return new string[] { password_ };
        }

        #endregion
    }
}
