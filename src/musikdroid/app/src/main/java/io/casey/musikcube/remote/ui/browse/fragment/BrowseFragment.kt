package io.casey.musikcube.remote.ui.browse.fragment

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.viewpager2.widget.ViewPager2
import com.google.android.material.floatingactionbutton.FloatingActionButton
import com.google.android.material.tabs.TabLayout
import com.google.android.material.tabs.TabLayoutMediator
import io.casey.musikcube.remote.R
import io.casey.musikcube.remote.ui.browse.adapter.BrowseFragmentAdapter
import io.casey.musikcube.remote.ui.browse.constant.Browse
import io.casey.musikcube.remote.ui.shared.activity.IFabConsumer
import io.casey.musikcube.remote.ui.shared.activity.IFilterable
import io.casey.musikcube.remote.ui.shared.activity.ITitleProvider
import io.casey.musikcube.remote.ui.shared.activity.ITransportObserver
import io.casey.musikcube.remote.ui.shared.fragment.BaseFragment
import io.casey.musikcube.remote.ui.shared.mixin.PlaybackMixin

class BrowseFragment: BaseFragment(), ITransportObserver, IFilterable, ITitleProvider {
    private lateinit var adapter: BrowseFragmentAdapter
    private lateinit var playback: PlaybackMixin

    override val title: String
        get() = getString(R.string.app_name)

    override fun onCreate(savedInstanceState: Bundle?) {
        playback = mixin(PlaybackMixin())
        super.onCreate(savedInstanceState)
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View? =
        inflater.inflate(R.layout.browse_fragment, container, false).apply {
            val fab = findViewById<FloatingActionButton>(R.id.fab)
            val pager = findViewById<ViewPager2>(R.id.view_pager)
            val tabs = findViewById<TabLayout>(R.id.tab_layout)

            val showFabIfNecessary = { pos: Int ->
                adapter.fragmentAt(pos)?.let {
                    when (it is IFabConsumer) {
                        true -> {
                            when (it.fabVisible) {
                                true -> fab.show()
                                false -> fab.hide()
                            }
                        }
                        false -> fab.hide()
                    }
                }
            }

            fab.setOnClickListener {
                (adapter.fragmentAt(pager.currentItem) as? IFabConsumer)?.onFabPress(fab)
            }

            adapter = BrowseFragmentAdapter(
                appCompatActivity,
                playback,
                this@BrowseFragment,
                R.id.content_container)

            adapter.onFragmentInstantiated = { pos ->
                if (pos == pager.currentItem) {
                    showFabIfNecessary(pos)
                }
            }

            pager.isSaveEnabled = false
            pager.adapter = adapter
            pager.offscreenPageLimit = adapter.itemCount

            pager.registerOnPageChangeCallback(object: ViewPager2.OnPageChangeCallback() {
                override fun onPageSelected(position: Int) {
                    showFabIfNecessary(position)
                }
            })

            TabLayoutMediator(tabs, pager, true) { tab, position ->
                tab.text = adapter.getPageTitle(position)
            }.attach()

            val initialIndex = adapter.indexOf(
                extras.getString(Browse.Extras.INITIAL_CATEGORY_TYPE))

            pager.setCurrentItem(initialIndex, false)

            pager.post {
                showFabIfNecessary(pager.currentItem)
            }
        }

    override fun onTransportChanged() =
        adapter.onTransportChanged()

    override val addFilterToToolbar: Boolean
        get() = true

    override fun setFilter(filter: String) {
        adapter.filter = filter
    }

    companion object {
        const val TAG = "BrowseFragment"

        fun create(extras: Bundle): BrowseFragment =
            BrowseFragment().apply { arguments = extras }
    }
}