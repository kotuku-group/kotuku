
class extImage : public objImage {
   public:
   std::string prvAuthor;
   std::string prvCopyright;
   std::string prvTitle;
   std::string prvSoftware;
   std::string prvDescription;
   std::string prvDisclaimer;
   int FrameRate;
   int8_t  prvHeader[256];
   objFile *prvFile;
   uint8_t Cached:1;
   uint8_t Queried:1;
};
