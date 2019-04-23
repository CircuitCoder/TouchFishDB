#include "engine_race.h"
#include <iostream>
#include <filesystem>

using namespace polar_race;
using namespace std;

int main() {
  Engine *r;

  std::filesystem::create_directory("./test");
  EngineRace::Open("test", &r);

  while(true) {
    cout<<"> "<<flush;
    string command;
    cin>>command;

    if(command == "quit") break;

    if(command == "set") {
      string k, v;
      cin>>k>>v;

      r->Write(k, v);
        cout<<"Out: Ok"<<endl;
    } else if(command == "get") {
      string k, v;
      cin>>k;

      auto result = r->Read(k, &v);

      if(result == kNotFound) {
        cout<<"Err: Not Found"<<endl;
      } else {
        cout<<"Out: "<<v<<endl;
      }
    } else {
      cout<<"Err: Unknown command: "<<command<<endl;
    }
  }

  delete r;
}
