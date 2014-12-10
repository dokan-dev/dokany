using System;
using System.Collections.Generic;
using System.Xml.Serialization;
using System.IO;
using System.Windows.Forms;

namespace DokanSSHFS
{
    public class Setting
    {
        public string Name = "New Setting";
        public string Host = "";
        public int Port = 22;
        public string User = "";
        public string PrivateKey = "";
        public bool UsePassword = false;
        public string ServerRoot = "/";
        public string Drive = "N";
        public bool DisableCache = false;
        public bool WithoutOfflineAttribute = false;
    }

    public class Settings
    {
        private List<Setting> settings_ = new List<Setting>();

        public Setting this[int index]
        {
            get
            {
                while(settings_.Count - 1 < index)
                {
                    settings_.Add(new Setting());
                }
                return settings_[index];
            }

        }

        public string GetNewName()
        {
            int index = 1;
            while (true)
            {
                string name = "Setting" + index.ToString();
                bool ok = true;
                foreach (Setting s in settings_)
                {
                    if (s.Name == name)
                        ok = false;
                }
                if (ok)
                    return name;
                index++;
            }
        }

        public int Count
        {
            get
            {
                return settings_.Count;
            }
        }

        public void Delete(int index)
        {
            if (0 <= index && index < settings_.Count)
                settings_.RemoveAt(index);
        }

        public void Save()
        {
            XmlSerializer serializer = new XmlSerializer(settings_.GetType());
            using (FileStream stream = new FileStream(Application.UserAppDataPath + "\\setting.xml", FileMode.Create))
            {
                serializer.Serialize(stream, settings_);
            }
        }

        public void Load()
        {
            try
            {
                XmlSerializer serializer = new XmlSerializer(settings_.GetType());
                using (FileStream stream = new FileStream(Application.UserAppDataPath + "\\setting.xml", FileMode.OpenOrCreate))
                {
                    settings_ = (List<Setting>)serializer.Deserialize(stream);
                }
            }
            catch (Exception)
            {
            }
        }
    }
}
