#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include "keybinding_config.h"
#include "gtkmm2ext/keyboard.h"

using namespace Gtkmm2ext;
using namespace ArdourKeybindingConfig;
#define CHECK(x) do { if (!(x)) throw std::runtime_error (#x); } while (0)

static std::string document (std::string const& body, std::string const& group = "Global")
{
	return "<BindingSet><Bindings name=\"" + group + "\"><Press>" + body + "</Press></Bindings></BindingSet>";
}

static std::string binding (std::string const& key, std::string const& action = "Main/AddTrackBus")
{
	return "<Binding key=\"" + key + "\" action=\"" + action + "\"/>";
}

static bool bound (Bindings const& bindings, std::string const& name)
{
	KeyboardKey key;
	CHECK (KeyboardKey::make_key (name, key));
	return bindings.is_bound (key, Bindings::Press);
}

int main (int argc, char** argv)
{
	try {
		Keyboard::RelevantModifierKeyMask = GdkModifierType (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_MOD2_MASK | GDK_MOD3_MASK | GDK_MOD4_MASK | GDK_MOD5_MASK);
		Bindings global ("Global");
		Bindings editor ("Editor");
		XMLTree initial;
		CHECK (initial.read_buffer (document (binding ("n") + binding ("e", "Main/Other")).c_str ()));
		global.load (*initial.root ()->children ().front ());
		editor.load (*initial.root ()->children ().front ());
		std::string error;
		CHECK (reload (document (binding ("F12")), error));
		CHECK (Bindings::get_bindings ("Global") == &global);
		CHECK (bound (global, "F12") && !bound (global, "n") && !bound (global, "e"));
		CHECK (bound (editor, "n"));

		std::vector<std::string> bad = {
			"<BindingSet>", "<Other/>", "<BindingSet/>",
			document (binding ("not-a-real-key")),
			document (binding ("Banana-n")),
			document (binding ("n") + binding ("n", "Main/Other")),
			document (binding ("n") + binding ("m")),
			document (binding ("n", "Main/Typo")),
			document (binding ("n"), "Typo"),
			"<BindingSet><Bindings name=\"Global\"><Press>" + binding ("x") + "</Press></Bindings><Bindings name=\"Editor\"><Press>" + binding ("unknown-key") + "</Press></Bindings></BindingSet>",
			"<BindingSet><Bindings name=\"Global\"><Press/><Press/></Bindings></BindingSet>"
		};
		for (auto const& xml : bad) {
			CHECK (!reload (xml, error));
			CHECK (!error.empty ());
			CHECK (bound (global, "F12") && !bound (global, "x"));
		}
		CHECK (reload (document (binding ("n")), error));
		CHECK (bound (global, "n") && !bound (global, "F12"));
		CHECK (reload (document ("<!-- editable shortcut -->" + binding ("n")), error));

		Bindings editing ("Editing");
		XMLTree inherited;
		CHECK (inherited.read_buffer (document (binding ("n", "/example"), "Editing").c_str ()));
		editing.load (*inherited.root ()->children ().front ());
		Bindings child ("Derived", editing);
		CHECK (reload (document (binding ("m", "/example"), "Editing"), error));
		CHECK (bound (child, "m") && !bound (child, "n"));
		CHECK (reload (document ("", "Editing"), error));
		CHECK (child.empty_keys ());
		CHECK (reload (document (binding ("n", "/example"), "Editing"), error));
		CHECK (bound (child, "n"));

		MouseButton mouse;
		CHECK (MouseButton::make_button ("1", mouse));
		global.add (mouse, Bindings::Press, "Main/AddTrackBus", nullptr);
		CHECK (!global.empty_mouse ());
		CHECK (reload (document (binding ("n")), error));
		CHECK (global.empty_mouse ());

		WatchState state;
		CHECK (!state.ready ("first") && state.ready ("first") && !state.ready ("first"));
		CHECK (!state.ready ("partial") && !state.ready ("complete") && state.ready ("complete"));
		state.missing ();
		CHECK (!state.ready ("complete") && state.ready ("complete"));

		if (argc > 1) {
			std::ifstream file (argv[1]);
			CHECK (file.good ());
			std::string xml ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
			XMLTree real;
			CHECK (real.read_buffer (xml.c_str ()));
			std::vector<std::unique_ptr<Bindings> > extra;
			for (XMLNode* node : real.root ()->children ()) {
				std::string name = node->property ("name")->value ();
				Bindings* live = Bindings::get_bindings (name);
				if (!live) {
					extra.emplace_back (new Bindings (name));
					live = extra.back ().get ();
				}
				live->load (*node);
			}
			if (!reload (xml, error)) throw std::runtime_error (error);
			CHECK (bound (global, "n"));
		}
		std::cout << "PASS: live replacement, stable references, transactional validation, inherited bindings, mouse removal, save debounce, and user config\n";
		return 0;
	} catch (std::exception const& error) {
		std::cerr << "FAIL: " << error.what () << '\n';
		return 1;
	}
}
